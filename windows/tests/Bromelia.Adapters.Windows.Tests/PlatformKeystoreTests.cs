using System.Diagnostics;
using System.Runtime.Versioning;
using System.Text;
using Bromelia.Foundation;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Adapters.Windows.Tests;

/// <summary>shared/fixtures/adapters/keystore.cases.json against Credential Manager and against the fallback files.</summary>
[SupportedOSPlatform("windows")]
public sealed class PlatformKeystoreTests
{
    static string? Text(JsonValue? v) =>
        v is null || v.IsNull ? null
        : v.AsString ?? new StringBuilder().Insert(0, v["repeat"]!.AsString!, (int)v["times"]!.AsInteger!.Value).ToString();

    static void RunAll(bool systemStore)
    {
        RunCases("adapters/keystore.cases.json", (id, given, expect) =>
        {
            var service = "BromeliaTest-" + Guid.NewGuid().ToString("N");
            var dir = Path.Combine(Path.GetTempPath(), service);
            var keystore = new PlatformKeystore(dir, service, systemStore);
            var steps = given["steps"]!.AsArray!.ToArray();
            var results = expect["results"]!.AsArray!.ToArray();
            try
            {
                for (var i = 0; i < steps.Length; i++)
                {
                    var name = steps[i]["name"]!.AsString!;
                    var want = results[i];
                    try
                    {
                        string? got = null;
                        switch (steps[i]["op"]!.AsString)
                        {
                            case "get": got = keystore.Get(name); break;
                            case "set": keystore.Set(name, Text(steps[i]["value"])!); break;
                            case "remove": keystore.Remove(name); break;
                        }
                        Assert.True(want["error"] == null, $"step {i}: expected {want}");
                        Assert.Equal(Text(want), got);
                    }
                    catch (BroFailure e)
                    {
                        Assert.Equal(want["error"]?.AsString, e.Error.Code);
                        Assert.Equal(name, e.Error.Params["name"]!.AsString);
                    }
                }
            }
            finally
            {
                foreach (var step in steps)
                    try { keystore.Remove(step["name"]!.AsString!); } catch (BroFailure) { }
                if (Directory.Exists(dir)) Directory.Delete(dir, true);
            }
            return true;
        });
    }

    /// <summary>Whether this logon session can keep credentials, asked of cmdkey (an SSH key logon can't: the keystore
    /// must then use its files).</summary>
    static bool CredentialManagerUsable()
    {
        var target = "BromeliaTest-" + Guid.NewGuid().ToString("N");
        var p = Process.Start(new ProcessStartInfo("cmdkey", $"/generic:{target} /user:probe /pass:probe")
            { RedirectStandardOutput = true, UseShellExecute = false })!;
        p.StandardOutput.ReadToEnd();
        p.WaitForExit();
        if (p.ExitCode != 0) return false;
        CmdKey("/delete:" + target);
        return true;
    }

    [Fact]
    public void TheSharedCasesPassInCredentialManager()
    {
        if (!OperatingSystem.IsWindows()) return;
        if (!CredentialManagerUsable())
        {
            Assert.Equal("file", new PlatformKeystore(Path.GetTempPath(), "BromeliaTest-" + Guid.NewGuid().ToString("N")).Backend());
            return;
        }
        RunAll(systemStore: true);
    }

    [Fact]
    public void TheSharedCasesPassInTheFallbackFiles()
    {
        if (!OperatingSystem.IsWindows()) return;
        RunAll(systemStore: false);
    }

    /// <summary>cmdkey lists the credential while it is set; the fallback folder stays empty.</summary>
    [Fact]
    public void SecretsGoToCredentialManager()
    {
        if (!OperatingSystem.IsWindows()) return;
        var service = "BromeliaTest-" + Guid.NewGuid().ToString("N");
        var dir = Path.Combine(Path.GetTempPath(), service);
        var keystore = new PlatformKeystore(dir, service);
        if (!CredentialManagerUsable()) return; // checked by TheSharedCasesPassInCredentialManager
        Assert.Equal("credentialManager", keystore.Backend());
        keystore.Set("metadata.tmdb", "abc123");
        try
        {
            Assert.Contains(service + ":metadata.tmdb", CmdKey("/list:" + service + "*"));
            Assert.False(Directory.Exists(dir));
            Assert.Equal("abc123", new PlatformKeystore(dir, service).Get("metadata.tmdb")); // another instance reads it
        }
        finally { keystore.Remove("metadata.tmdb"); }
        Assert.DoesNotContain(service + ":metadata.tmdb", CmdKey("/list:" + service + "*"));
    }

    [Fact]
    public void TheFallbackIsOneFilePerSecret()
    {
        if (!OperatingSystem.IsWindows()) return;
        var dir = Path.Combine(Path.GetTempPath(), "BromeliaTest-" + Guid.NewGuid().ToString("N"));
        var keystore = new PlatformKeystore(dir, systemStore: false);
        Assert.Equal("file", keystore.Backend());
        keystore.Set("metadata.tmdb", "abc123");
        try
        {
            Assert.Equal(new[] { "metadata.tmdb" }, Directory.GetFiles(dir).Select(Path.GetFileName).ToArray());
            Assert.Equal("abc123", File.ReadAllText(Path.Combine(dir, "metadata.tmdb")));
            // Only the current user and SYSTEM can open them, whatever the folder above allows.
            Assert.True(OwnerOnly.Holds(dir));
            Assert.True(OwnerOnly.Holds(Path.Combine(dir, "metadata.tmdb")));
        }
        finally { Directory.Delete(dir, true); }
    }

    static string CmdKey(string arguments)
    {
        var p = Process.Start(new ProcessStartInfo("cmdkey", arguments) { RedirectStandardOutput = true, UseShellExecute = false })!;
        var text = p.StandardOutput.ReadToEnd();
        p.WaitForExit();
        return text;
    }
}
