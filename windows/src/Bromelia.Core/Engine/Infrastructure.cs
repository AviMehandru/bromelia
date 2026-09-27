using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Text;
using Bromelia.Core.Logic;

namespace Bromelia.Core.Engine;

/// <summary>Minimal INotifyPropertyChanged base (no toolkit dependency).</summary>
public abstract class ObservableObject : INotifyPropertyChanged
{
    public event PropertyChangedEventHandler? PropertyChanged;

    protected bool Set<T>(ref T field, T value, [CallerMemberName] string? name = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value)) return false;
        field = value;
        OnPropertyChanged(name);
        return true;
    }

    protected void OnPropertyChanged([CallerMemberName] string? name = null) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
}

/// <summary>
/// Marshals work to the UI thread. The engine captures the SynchronizationContext it was created on
/// (the WinUI DispatcherQueue context) and posts process output there, preserving order.
/// </summary>
public sealed class UiDispatcher
{
    readonly SynchronizationContext? _context;

    public UiDispatcher(SynchronizationContext? context = null)
    {
        _context = context ?? SynchronizationContext.Current;
    }

    public void Post(Action action)
    {
        if (_context == null) { lock (this) action(); }
        else _context.Post(_ => action(), null);
    }

    /// <summary>Completes after every action posted before it has run.</summary>
    public Task Barrier()
    {
        var tcs = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Post(() => tcs.SetResult());
        return tcs.Task;
    }
}

public static class Paths
{
    /// <summary>Overrides the data folder (tests, or BROMELIA_DATA_DIR in the environment).</summary>
    public static string? DataOverride { get; set; } = Environment.GetEnvironmentVariable("BROMELIA_DATA_DIR");

    public static string AppData => DataOverride is { Length: > 0 } o ? o
        : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Bromelia");

    public static string ConfigFile => Path.Combine(AppData, "config.json");
    public static string HistoryFile => Path.Combine(AppData, "history.json");
    public static string JobsDirectory => Path.Combine(AppData, "jobs");

    public static string DefaultOutputRoot =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyVideos), "Bromelia");

    /// <summary>MakeMKV's settings folder for non-Windows systems (settings live in the registry on Windows).</summary>
    public static string MakemkvUserFolder
    {
        get
        {
            var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            return OperatingSystem.IsMacOS() ? Path.Combine(home, "Library", "MakeMKV") : Path.Combine(home, ".MakeMKV");
        }
    }

    public static string ExpandUser(string p)
    {
        p = Environment.ExpandEnvironmentVariables(p.Trim());
        if (p.StartsWith("~/") || p == "~")
            p = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), p.Length > 2 ? p[2..] : "");
        return p;
    }

    public static IEnumerable<string> MakemkvconCandidates()
    {
        if (OperatingSystem.IsWindows())
        {
            foreach (var pf in new[] { Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles) })
            {
                if (string.IsNullOrEmpty(pf)) continue;
                yield return Path.Combine(pf, "MakeMKV", "makemkvcon64.exe");
                yield return Path.Combine(pf, "MakeMKV", "makemkvcon.exe");
            }
        }
        else if (OperatingSystem.IsMacOS())
        {
            yield return "/Applications/MakeMKV.app/Contents/MacOS/makemkvcon";
            yield return "/opt/homebrew/bin/makemkvcon";
        }
        yield return "/usr/bin/makemkvcon";
        yield return "/usr/local/bin/makemkvcon";
    }

    public static IEnumerable<string> MkvmergeCandidates()
    {
        if (OperatingSystem.IsWindows())
        {
            foreach (var pf in new[] { Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86) })
                if (!string.IsNullOrEmpty(pf)) yield return Path.Combine(pf, "MKVToolNix", "mkvmerge.exe");
        }
        yield return "/opt/homebrew/bin/mkvmerge";
        yield return "/usr/local/bin/mkvmerge";
        yield return "/usr/bin/mkvmerge";
    }

    public static string? ResolveTool(string configured, IEnumerable<string> candidates, string exeName)
    {
        var c = ExpandUser(configured);
        if (c.Length > 0) return File.Exists(c) ? c : null;
        foreach (var cand in candidates) if (File.Exists(cand)) return cand;
        // Search PATH.
        foreach (var dir in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(Path.PathSeparator))
        {
            if (dir.Length == 0) continue;
            var p = Path.Combine(dir, exeName);
            if (File.Exists(p)) return p;
            if (OperatingSystem.IsWindows() && File.Exists(p + ".exe")) return p + ".exe";
        }
        return null;
    }

    public static string UniquePath(string path)
    {
        if (!File.Exists(path) && !Directory.Exists(path)) return path;
        var dir = Path.GetDirectoryName(path) ?? "";
        var ext = Path.GetExtension(path);
        var isDir = Directory.Exists(path) || ext.Length == 0;
        var stem = isDir ? Path.GetFileName(path) : Path.GetFileNameWithoutExtension(path);
        for (int n = 2; ; n++)
        {
            var cand = Path.Combine(dir, isDir ? $"{stem} ({n})" : $"{stem} ({n}){ext}");
            if (!File.Exists(cand) && !Directory.Exists(cand)) return cand;
        }
    }
}

/// <summary>Runs a child process and streams stdout/stderr lines.</summary>
public sealed class ProcessRunner
{
    public sealed record Result(int ExitCode, bool Cancelled, bool TimedOut);

    readonly ProcessStartInfo _psi;
    Process? _process;
    volatile bool _cancelled;
    volatile bool _timedOut;

    public ProcessRunner(string executable, IEnumerable<string> arguments, IDictionary<string, string>? environment = null, string? workingDirectory = null)
    {
        _psi = new ProcessStartInfo(executable)
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            RedirectStandardInput = true,
            UseShellExecute = false,
            CreateNoWindow = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
        };
        foreach (var a in arguments) _psi.ArgumentList.Add(a);
        if (environment != null) foreach (var kv in environment) _psi.Environment[kv.Key] = kv.Value;
        if (!string.IsNullOrEmpty(workingDirectory) && Directory.Exists(workingDirectory)) _psi.WorkingDirectory = workingDirectory;
    }

    public string CommandLine => string.Join(" ", new[] { _psi.FileName }.Concat(_psi.ArgumentList).Select(ArgumentSplitter.Quote));

    /// <summary>Raised on a thread-pool thread once the process has started.</summary>
    public event Action? Started;

    public async Task<Result> RunAsync(Action<string> onLine, TimeSpan timeout = default, CancellationToken ct = default)
    {
        var p = new Process { StartInfo = _psi };
        _process = p;
        p.Start();
        p.StandardInput.Close();
        Started?.Invoke();
        var readOut = Pump(p.StandardOutput, onLine);
        var readErr = Pump(p.StandardError, onLine);
        using var reg = ct.Register(Cancel);
        using var timer = timeout > TimeSpan.Zero ? new Timer(_ => { _timedOut = true; Kill(); }, null, timeout, Timeout.InfiniteTimeSpan) : null;
        await p.WaitForExitAsync().ConfigureAwait(false);
        await Task.WhenAll(readOut, readErr).ConfigureAwait(false);
        return new Result(p.ExitCode, _cancelled, _timedOut);
    }

    static async Task Pump(StreamReader reader, Action<string> onLine)
    {
        string? line;
        while ((line = await reader.ReadLineAsync().ConfigureAwait(false)) != null)
            if (line.Length > 0) onLine(line);
    }

    public void Cancel()
    {
        _cancelled = true;
        Kill();
    }

    void Kill()
    {
        try
        {
            if (_process is { HasExited: false } p) p.Kill(entireProcessTree: true);
        }
        catch (InvalidOperationException) { }
        catch (System.ComponentModel.Win32Exception) { }
    }
}

/// <summary>Thread-safe line collector.</summary>
public sealed class LineCollector
{
    readonly List<string> _lines = new();
    public void Add(string l) { lock (_lines) _lines.Add(l); }
    public List<string> All { get { lock (_lines) return new List<string>(_lines); } }
    public string Joined => string.Join("\n", All);
}
