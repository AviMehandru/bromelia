using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.Json;
using Bromelia.Core.Config;
using Bromelia.Core.Engine;
using Bromelia.Core.Robot;
using Xunit;

namespace Bromelia.Core.Tests;

public class WebApiTests
{
    [Fact]
    public void ParsesRequests()
    {
        Assert.Null(HttpRequest.Parse("GET / HTTP/1.1\r\nHost: x\r\n"u8));
        var r = HttpRequest.Parse("post /api/drives/dev%3A%2Fdev%2Fsr0/rip?titles=1%2C2&token=a+b HTTP/1.1\r\nHost: localhost:51280\r\nX-Bromelia: 1\r\n\r\n"u8)!;
        Assert.Equal("POST", r.Method);
        Assert.Equal("/api/drives/dev%3A%2Fdev%2Fsr0/rip", r.Path);
        Assert.Equal("1,2", r.Query["titles"]);
        Assert.Equal("a b", r.Query["token"]);
        Assert.Equal("localhost:51280", r.Headers["host"]);
        Assert.Equal("1", r.Headers["x-bromelia"]);
    }

    [Fact]
    public void LogTail()
    {
        var file = Path.Combine(Path.GetTempPath(), $"bromelia-log-{Guid.NewGuid():N}.txt");
        File.WriteAllText(file, string.Concat(Enumerable.Range(1, 1000).Select(n => $"line {n}\n")));
        try
        {
            Assert.StartsWith("line 1\n", WebLog.Tail(file));
            var tail = WebLog.Tail(file, 100);
            Assert.StartsWith("…\nline ", tail);
            Assert.EndsWith("line 1000\n", tail);
            Assert.True(tail.Length < 110);
            Assert.Equal("", WebLog.Tail("/nonexistent/log.txt"));
        }
        finally { File.Delete(file); }
    }

    [Fact]
    public void TitlesSettingsAndLogs()
    {
        var dir = Path.Combine(Path.GetTempPath(), "bromelia-webapi-" + Guid.NewGuid().ToString("N")[..8]);
        Directory.CreateDirectory(dir);
        Paths.DataOverride = dir;
        try
        {
            SingleThreadContext.Run(async () =>
            {
                var state = new AppState(new NullPlatformServices(), Path.Combine(dir, "config.json"));
                // No makemkvcon: a job fails at once instead of touching a drive.
                state.Config.MakemkvconPath = Path.Combine(dir, "missing-makemkvcon");
                var e = new DriveScanEntry(0, DriveState.Inserted, DiscFlags.DvdFiles, "BD-RE TEST DRIVE", "MOVIE_DISC", "/dev/sr99");
                state.ApplyScan(new[] { e });
                state.ApplyScan(new[] { e });
                var lane = e.LaneKey;
                Assert.Equal("Open the disc first", state.WebAction("drives", lane, "rip", new Dictionary<string, string> { ["titles"] = "1" }));
                var s = state.Sessions[lane];
                s.Info = DiscInfoBuilder.Build(Fixtures.Read("info-dvd"));
                s.SelectedTitles.Add(0);
                var drive = ((System.Collections.IEnumerable)state.WebStatus()["drives"]).Cast<Dictionary<string, object>>().First();
                var titles = ((System.Collections.IEnumerable)((Dictionary<string, object>)drive["opened"])["titles"]).Cast<Dictionary<string, object>>().ToList();
                Assert.Equal(s.Info.Titles.Count, titles.Count);
                Assert.True((bool)titles[0]["selected"]);
                Assert.Equal("No titles chosen", state.WebAction("drives", lane, "rip", new Dictionary<string, string> { ["titles"] = "999" }));
                var chosen = s.Info.Titles.Select(t => t.Index).TakeLast(2).ToList();
                Assert.Null(state.WebAction("drives", lane, "rip", new Dictionary<string, string> { ["titles"] = string.Join(",", chosen) }));
                var job = state.Jobs.Last();
                Assert.Equal(chosen, job.ManualTitles);
                Assert.Equal(RipMode.Mkv, job.Mode);
                await Task.Delay(100);
                Assert.NotNull(state.WebLog(job.Id.ToString()));
                Assert.Null(state.WebLog("nope"));

                var settings = (Dictionary<string, object>)state.WebStatus()["settings"];
                var id = ((System.Collections.IEnumerable)settings["drives"]).Cast<Dictionary<string, object>>().First()["id"].ToString()!;
                Assert.Null(state.WebAction("settings", id, "set", new Dictionary<string, string> { ["autoRip"] = "1", ["mode"] = "backupThenMkv" }));
                Assert.True(state.Config.DefaultDrive.Automation.AutoRipOnInsert);
                Assert.Equal(RipMode.BackupThenMkv, state.Config.DefaultDrive.Rip.Mode);
                Assert.Equal("Unknown mode “audioCD”", state.WebAction("settings", id, "set", new Dictionary<string, string> { ["mode"] = "audioCD" }));
                Assert.Equal("No such drive configuration", state.WebAction("settings", Guid.NewGuid().ToString(), "set"));

                state.Web.Apply(new WebUIConfig { Enabled = true, Port = Random.Shared.Next(52000, 58000) });
                try
                {
                    Assert.Equal(404, state.Web.Handle("GET", "/api/jobs/nope/log", "localhost", null, null, null).Item1);
                    var (code, type, body) = state.Web.Handle("GET", $"/api/jobs/{job.Id}/log", "localhost", null, null, null);
                    Assert.Equal((200, "text/plain; charset=utf-8"), (code, type));
                    Assert.Contains("\"settings\"", Encoding.UTF8.GetString(state.Web.Handle("GET", "/api/status", "localhost", null, null, null).Item3));
                }
                finally { state.Web.Stop(); }
            });
        }
        finally
        {
            Paths.DataOverride = null;
            try { Directory.Delete(dir, true); } catch (IOException) { }
        }
    }

    /// <summary>HTTPS with a PEM certificate and key; the round trip runs where connections to 127.0.0.1 work.</summary>
    [Fact]
    public void ServesHttps()
    {
        // Making a certificate on macOS needs a keychain the test can't use; this runs on Windows (and Linux).
        if (OperatingSystem.IsMacOS()) return;
        var dir = Path.Combine(Path.GetTempPath(), "bromelia-tls-" + Guid.NewGuid().ToString("N")[..8]);
        Directory.CreateDirectory(dir);
        Paths.DataOverride = dir;
        try
        {
            using var key = RSA.Create(2048);
            var req = new CertificateRequest("CN=localhost", key, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
            using var cert = req.CreateSelfSigned(DateTimeOffset.Now.AddDays(-1), DateTimeOffset.Now.AddDays(30));
            string certPath = Path.Combine(dir, "cert.pem"), keyPath = Path.Combine(dir, "key.pem");
            File.WriteAllText(certPath, cert.ExportCertificatePem());
            File.WriteAllText(keyPath, key.ExportPkcs8PrivateKeyPem());
            Assert.NotNull(WebAccess.StartProblem(new WebUIConfig { TlsCertificate = certPath }));
            using var loaded = WebServer.LoadCertificate(certPath, keyPath);
            Assert.True(loaded.HasPrivateKey);
            SingleThreadContext.Run(async () =>
            {
                var state = new AppState(new NullPlatformServices(), Path.Combine(dir, "config.json"));
                var port = Random.Shared.Next(52000, 58000);
                state.Web.Apply(new WebUIConfig { Enabled = true, Port = port, TlsCertificate = certPath, TlsKey = keyPath });
                Assert.Null(state.Web.LastError);
                try
                {
                    if (!await LoopbackWorks(port)) return;
                    using var handler = new HttpClientHandler { ServerCertificateCustomValidationCallback = (_, _, _, _) => true };
                    using var http = new HttpClient(handler) { Timeout = TimeSpan.FromSeconds(20) };
                    var status = await http.GetStringAsync($"https://localhost:{port}/api/status");
                    Assert.Contains("\"drives\"", status);
                }
                finally { state.Web.Stop(); }
            });
        }
        finally
        {
            Paths.DataOverride = null;
            try { Directory.Delete(dir, true); } catch (IOException) { }
        }
    }

    static async Task<bool> LoopbackWorks(int port)
    {
        using var c = new TcpClient();
        try { return await Task.WhenAny(c.ConnectAsync(IPAddress.Loopback, port), Task.Delay(2000)) is Task t && c.Connected; }
        catch (SocketException) { return false; }
    }
}
