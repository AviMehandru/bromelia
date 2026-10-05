using System.Net;
using System.Net.Sockets;
using System.Text;

namespace Bromelia.Adapters.Tests;

/// <summary>A tiny HTTP/1.1 server on 127.0.0.1 with the routes of http-client.cases.json: /echo, /status/n, /slow,
/// /redirect/path, /redirect-to-localhost/path, /big/n.</summary>
internal sealed class TestHttpServer : IDisposable
{
    readonly TcpListener _listener = new(IPAddress.Loopback, 0);
    readonly CancellationTokenSource _stop = new();

    public TestHttpServer()
    {
        _listener.Start();
        _ = Task.Run(Loop);
    }

    public string Url => "http://127.0.0.1:" + ((IPEndPoint)_listener.LocalEndpoint).Port;

    /// <summary>A port nothing listens on.</summary>
    public static string Closed()
    {
        var l = new TcpListener(IPAddress.Loopback, 0);
        l.Start();
        var port = ((IPEndPoint)l.LocalEndpoint).Port;
        l.Stop();
        return "http://127.0.0.1:" + port;
    }

    async Task Loop()
    {
        while (!_stop.IsCancellationRequested)
        {
            TcpClient client;
            try { client = await _listener.AcceptTcpClientAsync(_stop.Token); }
            catch (Exception) { return; }
            _ = Task.Run(() => Serve(client));
        }
    }

    static async Task Serve(TcpClient client)
    {
        using var _ = client;
        var stream = client.GetStream();
        var head = new StringBuilder();
        var buffer = new byte[1];
        while (!head.ToString().EndsWith("\r\n\r\n"))
        {
            if (await stream.ReadAsync(buffer) == 0) return;
            head.Append((char)buffer[0]);
        }
        var lines = head.ToString().Split("\r\n");
        var parts = lines[0].Split(' ');
        string? header = null, host = null;
        int length = 0;
        foreach (var l in lines.Skip(1))
        {
            var i = l.IndexOf(':');
            if (i < 0) continue;
            var name = l[..i].Trim().ToLowerInvariant();
            var value = l[(i + 1)..].Trim();
            if (name == "x-test") header = value;
            if (name == "host") host = value;
            if (name == "content-length") length = int.Parse(value);
        }
        var body = new byte[length];
        for (int got = 0; got < length;) got += await stream.ReadAsync(body.AsMemory(got));
        int status = 200;
        string text;
        string? location = null;
        if (parts[1].StartsWith("/big/"))
        {
            var n = int.Parse(parts[1]["/big/".Length..]);
            try
            {
                await stream.WriteAsync(Encoding.ASCII.GetBytes($"HTTP/1.1 200 X\r\nContent-Length: {n}\r\nConnection: close\r\n\r\n"));
                var x = new byte[65536];
                Array.Fill(x, (byte)'x');
                for (int sent = 0; sent < n; sent += x.Length) await stream.WriteAsync(x.AsMemory(0, Math.Min(x.Length, n - sent)));
            }
            catch (IOException) { }
            return;
        }
        if (parts[1].StartsWith("/redirect/"))
        {
            status = 302;
            location = "/" + parts[1]["/redirect/".Length..];
            text = "moved";
        }
        else if (parts[1].StartsWith("/redirect-to-localhost/"))
        {
            status = 302;
            location = "http://localhost:" + host!.Split(':')[1] + "/" + parts[1]["/redirect-to-localhost/".Length..];
            text = "moved";
        }
        else if (parts[1] == "/echo")
            text = "{\"method\":" + Json(parts[0]) + ",\"path\":\"/echo\",\"header\":" + (header is null ? "null" : Json(header)) + ",\"body\":" + Json(Encoding.UTF8.GetString(body)) + "}";
        else if (parts[1].StartsWith("/status/"))
        {
            status = int.Parse(parts[1]["/status/".Length..]);
            text = "status " + status;
        }
        else
        {
            await Task.Delay(5000);
            text = "slow";
        }
        var bytes = Encoding.UTF8.GetBytes(text);
        var response = Encoding.ASCII.GetBytes($"HTTP/1.1 {status} X\r\n{(location is null ? "" : $"Location: {location}\r\n")}Content-Length: {bytes.Length}\r\nConnection: close\r\n\r\n");
        try
        {
            await stream.WriteAsync(response);
            await stream.WriteAsync(bytes);
        }
        catch (IOException) { }
    }

    static string Json(string s) => System.Text.Json.JsonSerializer.Serialize(s);

    public void Dispose()
    {
        _stop.Cancel();
        _listener.Stop();
    }
}
