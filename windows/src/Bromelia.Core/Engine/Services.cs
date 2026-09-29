using System.Collections.ObjectModel;
using System.Net;
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

    public void Enqueue(BackgroundWork work)
    {
        Items.Add(new Item(work));
        Pump();
    }

    public void ClearFinished()
    {
        foreach (var i in Items.Where(i => i.State is "done" or "failed").ToList()) Items.Remove(i);
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
        });
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
        return null;
    }
}

/// <summary>Serves the web page (shared/web/bromelia-web.html, embedded) and its JSON API.</summary>
public sealed class WebServer
{
    readonly AppState _state;
    readonly UiDispatcher _ui;
    HttpListener? _listener;
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

    public void Apply(WebUIConfig config)
    {
        if (_listener != null && Running != null && JsonSerializer.Serialize(Running) == JsonSerializer.Serialize(config)) return;
        Stop();
        if (!config.Enabled) return;
        if (WebAccess.StartProblem(config) is { } p) { LastError = p; return; }
        try
        {
            var l = new HttpListener();
            var host = config.Address.Trim() is "0.0.0.0" or "*" ? "+" : config.Address.Trim();
            l.Prefixes.Add($"http://{host}:{config.Port}/");
            l.Start();
            _listener = l;
            Running = new WebUIConfig { Enabled = true, Address = config.Address, Port = config.Port, Token = config.Token };
            LastError = null;
            _ = LoopAsync(l);
        }
        catch (Exception e) when (e is HttpListenerException or InvalidOperationException or PlatformNotSupportedException)
        {
            LastError = $"Web page: {e.Message}";
        }
    }

    public void Stop()
    {
        try { _listener?.Close(); } catch (ObjectDisposedException) { }
        _listener = null;
        Running = null;
    }

    async Task LoopAsync(HttpListener l)
    {
        while (l.IsListening)
        {
            HttpListenerContext ctx;
            try { ctx = await l.GetContextAsync().ConfigureAwait(false); }
            catch (Exception) { return; }
            _ = HandleAsync(ctx);
        }
    }

    async Task HandleAsync(HttpListenerContext ctx)
    {
        var tcs = new TaskCompletionSource<(int, string, byte[])>();
        var req = ctx.Request;
        _ui.Post(() => tcs.TrySetResult(Handle(req.HttpMethod, req.Url?.AbsolutePath ?? "/", req.UserHostName, req.Headers["Authorization"],
            req.QueryString["token"], req.Headers["X-Bromelia"])));
        var (status, type, body) = await tcs.Task.ConfigureAwait(false);
        try
        {
            ctx.Response.StatusCode = status;
            ctx.Response.ContentType = type;
            ctx.Response.Headers["Cache-Control"] = "no-store";
            ctx.Response.Headers["X-Content-Type-Options"] = "nosniff";
            ctx.Response.ContentLength64 = body.Length;
            await ctx.Response.OutputStream.WriteAsync(body).ConfigureAwait(false);
            ctx.Response.Close();
        }
        catch (Exception) { }
    }

    /// <summary>Status code, content type and body for a request (call on the UI thread).</summary>
    public (int, string, byte[]) Handle(string method, string path, string? host, string? authorization, string? queryToken, string? xBromelia)
    {
        if (Running is not { } config) return (404, "text/plain", Array.Empty<byte>());
        var (ok, code, why) = WebAccess.Allowed(method.ToUpperInvariant(), host, authorization, queryToken, xBromelia, config);
        if (!ok) return (code, "text/plain; charset=utf-8", Encoding.UTF8.GetBytes(why));
        var parts = path.Split('/', StringSplitOptions.RemoveEmptyEntries).Select(Uri.UnescapeDataString).ToArray();
        switch (method.ToUpperInvariant(), parts.Length)
        {
            case ("GET", 0):
                return Page() is { } page ? (200, "text/html; charset=utf-8", page) : (404, "text/plain", Encoding.UTF8.GetBytes("The page is missing"));
            case ("GET", 2) when parts[0] == "api" && parts[1] == "status":
                return (200, "application/json", JsonSerializer.SerializeToUtf8Bytes(_state.WebStatus()));
            case ("POST", 4) when parts[0] == "api":
                var message = _state.WebAction(parts[1], parts[2], parts[3]);
                return message == null ? (204, "text/plain", Array.Empty<byte>()) : (400, "text/plain; charset=utf-8", Encoding.UTF8.GetBytes(message));
            default:
                return (404, "text/plain", Encoding.UTF8.GetBytes("Not found"));
        }
    }
}
