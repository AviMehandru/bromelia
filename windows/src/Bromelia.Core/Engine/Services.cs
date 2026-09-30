using System.Collections.ObjectModel;
using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Cryptography.X509Certificates;
using System.Reflection;
using System.Text;
using System.Text.Json;
using Bromelia.Core.Config;

namespace Bromelia.Core.Engine;

/// <summary>Runs post-processing steps marked "background" (encoding, uploads) after their job has finished and the disc
/// is out, a few at a time, so the drive is free for the next disc. Used on the UI thread.</summary>
public sealed class BackgroundQueue
{
    public sealed class Item : ObservableObject
    {
        string _state = "queued", _message = "";
        public Item(BackgroundWork work) => Work = work;
        public Guid Id { get; } = Guid.NewGuid();
        public BackgroundWork Work { get; }
        /// <summary>queued, running, done or failed.</summary>
        public string State { get => _state; set => Set(ref _state, value); }
        public string Message { get => _message; set => Set(ref _message, value); }
    }

    readonly UiDispatcher _ui;
    public BackgroundQueue(UiDispatcher ui) => _ui = ui;

    public ObservableCollection<Item> Items { get; } = new();
    public int Limit { get; set; } = 1;
    public bool IsBusy => Items.Any(i => i.State is "queued" or "running");
    /// <summary>Raised on the UI thread when an item is added, finishes or is cleared.</summary>
    public event Action? Changed;

    public void Enqueue(BackgroundWork work)
    {
        Items.Add(new Item(work));
        Pump();
        Changed?.Invoke();
    }

    public void ClearFinished()
    {
        foreach (var i in Items.Where(i => i.State is "done" or "failed").ToList()) Items.Remove(i);
        Changed?.Invoke();
    }

    void Pump()
    {
        while (Items.Count(i => i.State == "running") < Math.Max(1, Limit) && Items.FirstOrDefault(i => i.State == "queued") is { } next)
        {
            next.State = "running";
            _ = RunAsync(next);
        }
    }

    async Task RunAsync(Item item)
    {
        var log = new StringBuilder();
        var results = await PostProcessor.RunAsync(item.Work.Steps, item.Work.Context, _ => { }, (text, _) => { lock (log) log.AppendLine(text); }, default);
        try { File.WriteAllText(item.Work.LogFile, log.ToString()); } catch (IOException) { }
        var failed = results.Where(r => r.ExitCode != 0 || r.TimedOut).ToList();
        _ui.Post(() =>
        {
            item.State = failed.Count == 0 ? "done" : "failed";
            item.Message = failed.Count == 0 ? $"{results.Count} step(s) finished" : $"“{failed[0].Name}” failed; see {item.Work.LogFile}";
            Pump();
            Changed?.Invoke();
        });
    }
}

/// <summary>A parsed HTTP request (enough of HTTP/1.1 for the web page and its JSON API).</summary>
public sealed record HttpRequest(string Method, string Path, Dictionary<string, string> Query, Dictionary<string, string> Headers)
{
    /// <summary>Parses the request head. Returns null until the whole head (up to the blank line) has arrived, and for malformed
    /// requests. Header names are lower case; the path stays percent-encoded.</summary>
    public static HttpRequest? Parse(ReadOnlySpan<byte> data)
    {
        var end = data.IndexOf("\r\n\r\n"u8);
        if (end < 0) return null;
        var lines = Encoding.UTF8.GetString(data[..end]).Split("\r\n");
        var first = lines[0].Split(' ', StringSplitOptions.RemoveEmptyEntries);
        if (first.Length < 2) return null;
        var headers = new Dictionary<string, string>();
        foreach (var l in lines.Skip(1))
        {
            var colon = l.IndexOf(':');
            if (colon > 0) headers[l[..colon].Trim().ToLowerInvariant()] = l[(colon + 1)..].Trim();
        }
        var target = first[1];
        var q = target.IndexOf('?');
        var query = new Dictionary<string, string>();
        if (q >= 0)
        {
            foreach (var pair in target[(q + 1)..].Split('&', StringSplitOptions.RemoveEmptyEntries))
            {
                var eq = pair.IndexOf('=');
                var k = Uri.UnescapeDataString((eq < 0 ? pair : pair[..eq]).Replace('+', ' '));
                query[k] = eq < 0 ? "" : Uri.UnescapeDataString(pair[(eq + 1)..].Replace('+', ' '));
            }
            target = target[..q];
        }
        return new HttpRequest(first[0].ToUpperInvariant(), target, query, headers);
    }
}

/// <summary>The end of a log file for the web page: at most <paramref name="limit"/> bytes, from the start of a line.</summary>
public static class WebLog
{
    public static string Tail(string path, int limit = 256 * 1024)
    {
        try
        {
            using var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            var start = Math.Max(0, f.Length - limit);
            f.Seek(start, SeekOrigin.Begin);
            var buf = new byte[f.Length - start];
            int read = 0, n;
            while (read < buf.Length && (n = f.Read(buf, read, buf.Length - read)) > 0) read += n;
            var from = 0;
            if (start > 0) from = Array.IndexOf(buf, (byte)'\n', 0, read) + 1;
            return (start > 0 ? "…\n" : "") + Encoding.UTF8.GetString(buf, from, read - from);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return ""; }
    }
}

/// <summary>Who may use the web page: with a token, whoever sends it; without one, only pages on this computer (the Host
/// header must be localhost, which also defeats DNS rebinding). Actions need the X-Bromelia header, which a page on another
/// site can't send without Bromelia's permission (CORS).</summary>
public static class WebAccess
{
    public static (bool Ok, int Status, string Why) Allowed(string method, string? host, string? authorization, string? queryToken, string? xBromelia, WebUIConfig config)
    {
        var token = config.Token.Trim();
        if (token.Length > 0)
        {
            var bearer = authorization is { } a && a.StartsWith("Bearer ", StringComparison.Ordinal) ? a[7..] : "";
            if (bearer != token && queryToken != token) return (false, 401, "A token is required");
        }
        else
        {
            var h = (host ?? "").ToLowerInvariant();
            var name = h.StartsWith('[') ? h[..(h.IndexOf(']') + 1)] : h.Split(':')[0];
            if (name is not ("localhost" or "127.0.0.1" or "[::1]")) return (false, 403, "Set a token to use Bromelia from another computer");
        }
        if (method == "POST" && xBromelia != "1") return (false, 403, "Missing X-Bromelia header");
        return (true, 200, "");
    }

    /// <summary>A page for the network (not only this computer) must have a token.</summary>
    public static string? StartProblem(WebUIConfig config)
    {
        var loopback = config.Address.Trim() is "127.0.0.1" or "localhost" or "::1";
        if (!loopback && config.Token.Trim().Length == 0) return "The web page is reachable from the network only with a token. Set a token or use 127.0.0.1.";
        if (config.Port is < 1 or > 65535) return $"Port {config.Port} is not valid";
        if ((config.TlsCertificate.Trim().Length > 0) != (config.TlsKey.Trim().Length > 0)) return "HTTPS needs both the certificate and its key";
        return null;
    }
}

/// <summary>Serves the web page (shared/web/bromelia-web.html, embedded) and its JSON API over HTTP/1.1, or HTTPS with a PEM
/// certificate and key. One request per connection.</summary>
public sealed class WebServer
{
    readonly AppState _state;
    readonly UiDispatcher _ui;
    TcpListener? _listener;
    X509Certificate2? _certificate;
    public WebUIConfig? Running { get; private set; }
    public string? LastError { get; private set; }

    public WebServer(AppState state, UiDispatcher ui) { _state = state; _ui = ui; }

    public static byte[]? Page()
    {
        using var s = Assembly.GetExecutingAssembly().GetManifestResourceStream("Bromelia.Core.bromelia-web.html");
        if (s == null) return null;
        using var m = new MemoryStream();
        s.CopyTo(m);
        return m.ToArray();
    }

    /// <summary>The certificate and key for HTTPS, from PEM files.</summary>
    public static X509Certificate2 LoadCertificate(string certificate, string key)
    {
        var cert = X509Certificate2.CreateFromPemFile(Paths.ExpandUser(certificate.Trim()), Paths.ExpandUser(key.Trim()));
        // Windows' TLS needs the key in a key store, not only in memory.
        return OperatingSystem.IsWindows() ? new X509Certificate2(cert.Export(X509ContentType.Pkcs12)) : cert;
    }

    static IPAddress ListenAddress(string address) => address.Trim() switch
    {
        "localhost" => IPAddress.Loopback,
        "0.0.0.0" or "*" or "+" => IPAddress.Any,
        "::" => IPAddress.IPv6Any,
        var a => IPAddress.Parse(a),
    };

    public void Apply(WebUIConfig config)
    {
        if (_listener != null && Running != null && JsonSerializer.Serialize(Running) == JsonSerializer.Serialize(config)) return;
        Stop();
        if (!config.Enabled) return;
        if (WebAccess.StartProblem(config) is { } p) { LastError = p; return; }
        try
        {
            _certificate = config.TlsCertificate.Trim().Length > 0 ? LoadCertificate(config.TlsCertificate, config.TlsKey) : null;
            var l = new TcpListener(ListenAddress(config.Address), config.Port);
            l.Start();
            _listener = l;
            Running = new WebUIConfig { Enabled = true, Address = config.Address, Port = config.Port, Token = config.Token,
                                        TlsCertificate = config.TlsCertificate, TlsKey = config.TlsKey };
            LastError = null;
            _ = LoopAsync(l, _certificate);
        }
        catch (Exception e) when (e is SocketException or FormatException or IOException or UnauthorizedAccessException
                                   or System.Security.Cryptography.CryptographicException or ArgumentException)
        {
            LastError = $"Web page: {e.Message}";
            _certificate = null;
        }
    }

    public void Stop()
    {
        try { _listener?.Stop(); } catch (SocketException) { }
        _listener = null;
        Running = null;
    }

    async Task LoopAsync(TcpListener l, X509Certificate2? cert)
    {
        while (true)
        {
            TcpClient client;
            try { client = await l.AcceptTcpClientAsync().ConfigureAwait(false); }
            catch (Exception) { return; }
            _ = ServeAsync(client, cert);
        }
    }

    async Task ServeAsync(TcpClient client, X509Certificate2? cert)
    {
        using var _ = client;
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
        try
        {
            Stream stream = client.GetStream();
            if (cert != null)
            {
                var ssl = new SslStream(stream, false);
                await ssl.AuthenticateAsServerAsync(new SslServerAuthenticationOptions { ServerCertificate = cert }, timeout.Token).ConfigureAwait(false);
                stream = ssl;
            }
            await using (stream.ConfigureAwait(false))
            {
                var buf = new byte[65536];
                int len = 0;
                HttpRequest? r = null;
                while (r == null)
                {
                    if (len == buf.Length) return;
                    var n = await stream.ReadAsync(buf.AsMemory(len), timeout.Token).ConfigureAwait(false);
                    if (n <= 0) return;
                    len += n;
                    r = HttpRequest.Parse(buf.AsSpan(0, len));
                }
                var tcs = new TaskCompletionSource<(int, string, byte[])>(TaskCreationOptions.RunContinuationsAsynchronously);
                var request = r;
                _ui.Post(() => tcs.TrySetResult(Handle(request)));
                var (status, type, body) = await tcs.Task.ConfigureAwait(false);
                var head = $"HTTP/1.1 {status} {Reason(status)}\r\nContent-Type: {type}\r\nContent-Length: {body.Length}\r\nConnection: close\r\n" +
                           "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n\r\n";
                await stream.WriteAsync(Encoding.ASCII.GetBytes(head), timeout.Token).ConfigureAwait(false);
                await stream.WriteAsync(body, timeout.Token).ConfigureAwait(false);
                await stream.FlushAsync(timeout.Token).ConfigureAwait(false);
            }
        }
        catch (Exception e) when (e is IOException or SocketException or OperationCanceledException or System.Security.Authentication.AuthenticationException
                                   or ObjectDisposedException) { }
    }

    static string Reason(int status) => status switch
    {
        200 => "OK", 204 => "No Content", 400 => "Bad Request", 401 => "Unauthorized", 403 => "Forbidden", 404 => "Not Found", _ => "Error",
    };

    /// <summary>Status code, content type and body for a request (call on the UI thread).</summary>
    public (int, string, byte[]) Handle(HttpRequest r)
    {
        if (Running is not { } config) return (404, "text/plain", Array.Empty<byte>());
        r.Headers.TryGetValue("host", out var host);
        r.Headers.TryGetValue("authorization", out var authorization);
        r.Query.TryGetValue("token", out var queryToken);
        r.Headers.TryGetValue("x-bromelia", out var xBromelia);
        var (ok, code, why) = WebAccess.Allowed(r.Method, host, authorization, queryToken, xBromelia, config);
        if (!ok) return (code, "text/plain; charset=utf-8", Encoding.UTF8.GetBytes(why));
        var parts = r.Path.Split('/', StringSplitOptions.RemoveEmptyEntries).Select(Uri.UnescapeDataString).ToArray();
        switch (r.Method, parts.Length)
        {
            case ("GET", 0):
                return Page() is { } page ? (200, "text/html; charset=utf-8", page) : (404, "text/plain", Encoding.UTF8.GetBytes("The page is missing"));
            case ("GET", 2) when parts[0] == "api" && parts[1] == "status":
                return (200, "application/json", JsonSerializer.SerializeToUtf8Bytes(_state.WebStatus()));
            case ("GET", 4) when parts[0] == "api" && parts[1] == "jobs" && parts[3] == "log":
                return _state.WebLog(parts[2]) is { } log ? (200, "text/plain; charset=utf-8", Encoding.UTF8.GetBytes(log))
                    : (404, "text/plain; charset=utf-8", Encoding.UTF8.GetBytes("No such job"));
            // /api/<kind>/<id>/<action>, and /api/verify (check the output folder's archives) · /api/verify/cancel.
            case ("POST", >= 2 and <= 4) when parts[0] == "api" && (parts.Length == 4 || parts[1] == "verify"):
                var message = _state.WebAction(parts[1], parts.Length == 4 ? parts[2] : "",
                    parts.Length == 4 ? parts[3] : parts.Length == 3 ? parts[2] : "start", r.Query);
                return message == null ? (204, "text/plain", Array.Empty<byte>()) : (400, "text/plain; charset=utf-8", Encoding.UTF8.GetBytes(message));
            default:
                return (404, "text/plain", Encoding.UTF8.GetBytes("Not found"));
        }
    }

    /// <summary>A request built from its parts (for tests).</summary>
    public (int, string, byte[]) Handle(string method, string path, string? host, string? authorization, string? queryToken, string? xBromelia)
    {
        var headers = new Dictionary<string, string>();
        if (host != null) headers["host"] = host;
        if (authorization != null) headers["authorization"] = authorization;
        if (xBromelia != null) headers["x-bromelia"] = xBromelia;
        var r = HttpRequest.Parse(Encoding.UTF8.GetBytes($"{method} {path} HTTP/1.1\r\n\r\n"))!;
        var query = new Dictionary<string, string>(r.Query);
        if (queryToken != null) query["token"] = queryToken;
        return Handle(r with { Headers = headers, Query = query });
    }
}
