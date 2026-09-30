using System.Collections.Concurrent;
using System.Diagnostics;
using System.Runtime.InteropServices;
using Bromelia.Core.Config;
using Bromelia.Core.Engine;

namespace Bromelia.Daemon;

/// <summary>Command-line options, the same as bromelia-daemon on Linux and Bromelia --headless on macOS.</summary>
public sealed record Options(string? Config = null, string? Listen = null, int? Port = null, string? Token = null)
{
    public const string Usage = """
        Usage: bromelia-daemon [--config PATH] [--listen ADDRESS] [--port PORT] [--token TOKEN]
               bromelia-daemon --install-task [options]   run it at every logon (Task Scheduler)
               bromelia-daemon --uninstall-task
        Rips discs without a window, as the configuration says (the app writes it; see docs/configuration.md).
          --config PATH      configuration file (default: %LOCALAPPDATA%\Bromelia\config.json)
          --listen ADDRESS   serve the web page on this address (0.0.0.0 = the network; needs a token)
          --port PORT        port of the web page (default 51280)
          --token TOKEN      token for the web page (or BROMELIA_WEB_TOKEN)
        """;

    /// <summary>The options, or an error.</summary>
    public static (Options? Options, string? Error) Parse(IReadOnlyList<string> args)
    {
        var o = new Options();
        for (int i = 0; i < args.Count; i++)
        {
            var a = args[i];
            if (a is "--install-task") continue;
            if (a is not ("--config" or "-c" or "--listen" or "-l" or "--port" or "-p" or "--token" or "-t")) return (null, $"Unknown option {a}");
            if (i + 1 >= args.Count) return (null, $"{a} needs a value");
            var v = args[++i];
            switch (a)
            {
                case "--config" or "-c": o = o with { Config = v }; break;
                case "--listen" or "-l": o = o with { Listen = v }; break;
                case "--port" or "-p":
                    if (!int.TryParse(v, out var p) || p is < 1 or > 65535) return (null, $"Port {v} is not valid");
                    o = o with { Port = p };
                    break;
                default: o = o with { Token = v }; break;
            }
        }
        return (o, null);
    }
}

/// <summary>Task Scheduler: bromelia-daemon at every logon of this user (it needs the user's MakeMKV settings and drives).</summary>
public static class LogonTask
{
    public const string Name = "Bromelia";

    /// <summary>schtasks arguments that create the task.</summary>
    public static string[] CreateArguments(string exe, IEnumerable<string> options) =>
        new[] { "/Create", "/F", "/SC", "ONLOGON", "/RL", "LIMITED", "/TN", Name,
                "/TR", string.Join(" ", new[] { exe }.Concat(options).Select(Quote)) };

    static string Quote(string a) => a.Length > 0 && !a.Any(c => char.IsWhiteSpace(c) || c == '"') ? a : "\"" + a.Replace("\"", "\\\"") + "\"";

    public static int Run(params string[] args)
    {
        var psi = new ProcessStartInfo("schtasks.exe") { UseShellExecute = false };
        foreach (var a in args) psi.ArgumentList.Add(a);
        var p = Process.Start(psi);
        p!.WaitForExit();
        return p.ExitCode;
    }
}

/// <summary>Runs posted work on one thread, like a UI thread: the engine captures it as its SynchronizationContext.</summary>
sealed class Loop : SynchronizationContext
{
    readonly BlockingCollection<(SendOrPostCallback, object?)> _queue = new();
    public override void Post(SendOrPostCallback d, object? state) { if (!_queue.IsAddingCompleted) _queue.Add((d, state)); }
    public void Quit() => _queue.CompleteAdding();
    public void Run()
    {
        foreach (var (d, s) in _queue.GetConsumingEnumerable()) d(s);
    }
}

sealed class DaemonPlatform : WindowsDeviceServices
{
    public override void Notify(string title, string body, bool sound) => Program.Say($"{title}: {body}");
}

public static class Program
{
    static readonly object Out = new();
    static StreamWriter? _log;

    /// <summary>A line on the terminal (when there is one) and in daemon.log in the data folder.</summary>
    public static void Say(string text)
    {
        var line = $"{DateTime.Now:yyyy-MM-dd HH:mm:ss} {text}";
        lock (Out)
        {
            Console.WriteLine(line);
            try { _log?.WriteLine(line); } catch (IOException) { }
        }
    }

    [DllImport("kernel32.dll")]
    static extern bool AttachConsole(int processId);

    static void OpenLog()
    {
        try
        {
            Directory.CreateDirectory(Paths.AppData);
            var path = Path.Combine(Paths.AppData, "daemon.log");
            // Starts again when it has grown past 5 MB; the previous one is kept as daemon.old.log.
            if (File.Exists(path) && new FileInfo(path).Length > 5 << 20) File.Move(path, Path.Combine(Paths.AppData, "daemon.old.log"), true);
            _log = new StreamWriter(new FileStream(path, FileMode.Append, FileAccess.Write, FileShare.ReadWrite)) { AutoFlush = true };
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
    }

    public static int Main(string[] args)
    {
        // A GUI-subsystem program: use the terminal it was started from, if any.
        if (OperatingSystem.IsWindows()) AttachConsole(-1);
        var version = typeof(Program).Assembly.GetName().Version?.ToString(3) ?? "";
        if (args.Contains("--version")) { Console.WriteLine($"bromelia-daemon {version}"); return 0; }
        if (args.Contains("--help") || args.Contains("-h")) { Console.WriteLine(Options.Usage); return 0; }
        if (args.Contains("--uninstall-task")) return LogonTask.Run("/Delete", "/F", "/TN", LogonTask.Name);
        var (o, error) = Options.Parse(args);
        if (o == null) { Console.Error.WriteLine(error + "\n" + Options.Usage); return 2; }
        if (args.Contains("--install-task"))
        {
            var code = LogonTask.Run(LogonTask.CreateArguments(Environment.ProcessPath ?? "bromelia-daemon.exe", args.Where(a => a != "--install-task")));
            if (code == 0) Console.WriteLine($"bromelia-daemon now starts at every logon (task “{LogonTask.Name}”); its log is daemon.log in the data folder");
            return code;
        }
        return Run(o, version);
    }

    static int Run(Options o, string version)
    {
        OpenLog();
        var loop = new Loop();
        SynchronizationContext.SetSynchronizationContext(loop);
        var state = new AppState(new DaemonPlatform(), o.Config is { } c ? Paths.ExpandUser(c) : null);
        var token = o.Token ?? Environment.GetEnvironmentVariable("BROMELIA_WEB_TOKEN");
        if (o.Listen != null || o.Port != null || token != null)
        {
            // The file keeps its own web settings.
            state.SavedWebUI = System.Text.Json.JsonSerializer.Deserialize<WebUIConfig>(ConfigJson.Serialize(state.Config.WebUI), ConfigJson.Options);
            state.Config.WebUI.Enabled = true;
            if (o.Listen != null) state.Config.WebUI.Address = o.Listen;
            if (o.Port is { } p) state.Config.WebUI.Port = p;
            if (token != null) state.Config.WebUI.Token = token;
        }
        Say($"bromelia-daemon {version}; configuration {state.ConfigPath}");
        var exe = Paths.ResolveTool(state.Config.MakemkvconPath, Paths.MakemkvconCandidates(), "makemkvcon");
        Say($"makemkvcon: {exe ?? "not found — install MakeMKV or set makemkvconPath"}");
        Say($"Output folder: {state.Config.OutputRoot}");
        if (state.LastError is { } recovered) Say(recovered);
        state.ImportFromMakeMkvIfFirstRun();
        state.StartServices("the daemon");
        if (!state.OwnsAutomation) Say($"Another Bromelia ({state.AutomationHolder}) rips inserted discs; this one takes over when it quits");
        if (state.Config.WebUI.Enabled)
        {
            var w = state.Config.WebUI;
            Say(state.Web.LastError is { } e ? $"Web page: {e}" : $"Web page: {(w.TlsCertificate.Length > 0 ? "https" : "http")}://{w.Address}:{w.Port}/");
        }

        var stopping = false;
        var states = new Dictionary<Guid, JobState>();
        string? lastError = state.LastError;
        void Report()
        {
            foreach (var j in state.Jobs.ToList())
            {
                if (states.TryGetValue(j.Id, out var before) && before == j.State) continue;
                states[j.Id] = j.State;
                Say(j.State.IsFinished()
                    ? $"{j.Title}: {j.State.Label()}{(j.OutputDirectory is { } d ? " — " + d : "")}{(j.ErrorMessage is { } m ? " — " + m : "")}"
                    : $"{j.Title}: {j.State.Label()} ({j.Mode.Label()})");
            }
            if (state.LastError != lastError && state.LastError is { } err) Say(err);
            lastError = state.LastError;
            if (stopping && state.ActiveJobCount == 0) { state.SaveNow(); loop.Quit(); }
        }
        void Stop()
        {
            if (stopping) { state.SaveNow(); loop.Quit(); return; }
            stopping = true;
            Say("Stopping: cancelling running jobs (send the signal again to quit at once)");
            foreach (var j in state.Jobs.Where(j => !j.State.IsFinished()).ToList()) state.Cancel(j);
            Report();
        }
        var signals = new[] { PosixSignal.SIGINT, PosixSignal.SIGTERM }.Select(sig => PosixSignalRegistration.Create(sig, ctx =>
        {
            ctx.Cancel = true;
            loop.Post(_ => Stop(), null);
        })).ToList();

        // Once a second: the queue, countdowns and the report; drives when media change, and every poll interval.
        var media = WindowsDeviceServices.MediaSignature();
        var lastPoll = DateTime.Now;
        using var tick = new Timer(_ => loop.Post(_ =>
        {
            state.Pump();
            Report();
            var sig = WindowsDeviceServices.MediaSignature();
            var interval = Math.Max(3, state.Config.PollIntervalSeconds);
            if (sig != media) { media = sig; _ = state.RefreshDrivesAsync(true); lastPoll = DateTime.Now; }
            else if (state.Config.PollIntervalSeconds > 0 && (DateTime.Now - lastPoll).TotalSeconds >= interval) { _ = state.RefreshDrivesAsync(false); lastPoll = DateTime.Now; }
        }, null), null, 1000, 1000);
        loop.Post(_ => _ = state.RefreshDrivesAsync(true), null);
        loop.Run();
        foreach (var s in signals) s.Dispose();
        return 0;
    }
}
