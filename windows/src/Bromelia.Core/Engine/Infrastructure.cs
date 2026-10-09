using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Bromelia.Core.Config;
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

    /// <summary>A free name for something another program will create (MakeMKV makes a backup's destination itself, so
    /// nothing holds the name until it has): <c>&lt;stem&gt;&lt;ext&gt;</c>, else <c>&lt;stem&gt; (2)&lt;ext&gt;</c> and
    /// so on, held meanwhile by a hidden marker next to it (<c>.&lt;name&gt;.bromelia</c>, created exclusively), so that
    /// two backups started together can't pick the same one. Delete the marker once the thing has its name. No marker
    /// when the folder can't take one.</summary>
    public static (string Path, string? Marker) ReserveUnique(string dir, string stem, string ext)
    {
        for (int n = 1; ; n++)
        {
            var leaf = n == 1 ? stem + ext : $"{stem} ({n}){ext}";
            var path = Path.Combine(dir, leaf);
            var marker = Path.Combine(dir, $".{leaf}.bromelia");
            if (File.Exists(path) || Directory.Exists(path) || File.Exists(marker)) continue;
            try
            {
                using (new FileStream(marker, FileMode.CreateNew, FileAccess.Write)) { }
                try { File.SetAttributes(marker, File.GetAttributes(marker) | FileAttributes.Hidden); } catch (IOException) { }
            }
            catch (IOException) when (File.Exists(marker)) { continue; }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return (path, null); }
            if (File.Exists(path) || Directory.Exists(path))
            {
                try { File.Delete(marker); } catch (IOException) { }
                continue;
            }
            return (path, marker);
        }
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
    /// <param name="Stalled">Stopped because it printed nothing for the stall timeout.</param>
    /// <param name="Abandoned">It didn't exit even after being killed (e.g. stuck in a drive I/O call); we stopped waiting.</param>
    public sealed record Result(int ExitCode, bool Cancelled, bool TimedOut, bool Stalled = false, bool Abandoned = false);

    readonly ProcessStartInfo _psi;
    Process? _process;
    volatile bool _cancelled;
    volatile bool _timedOut;
    volatile bool _stalled;
    long _lastOutput;
    readonly TaskCompletionSource _killed = new(TaskCreationOptions.RunContinuationsAsynchronously);

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

    IntPtr _job;

    /// <summary>Whether the running process is in its kill-on-close job (<see cref="ToolJob"/>; tests).</summary>
    public bool InToolJob => _process is { } p && ToolJob.Holds(_job, p);

    /// <summary>Raised on a thread-pool thread once the process has started.</summary>
    public event Action? Started;

    /// <summary>Runs the process to completion. With <paramref name="stallTimeout"/>, the process is stopped when it prints
    /// nothing for that long. A process that is still there 30 s after being killed is abandoned so the caller can go on.</summary>
    public async Task<Result> RunAsync(Action<string> onLine, TimeSpan timeout = default, CancellationToken ct = default, TimeSpan stallTimeout = default)
    {
        var p = new Process { StartInfo = _psi };
        _process = p;
        p.Start();
        _job = ToolJob.Add(p);
        p.StandardInput.Close();
        Interlocked.Exchange(ref _lastOutput, Environment.TickCount64);
        Started?.Invoke();
        var readOut = Pump(p.StandardOutput, onLine);
        var readErr = Pump(p.StandardError, onLine);
        using var reg = ct.Register(Cancel);
        using var timer = timeout > TimeSpan.Zero ? new Timer(_ => { _timedOut = true; Kill(); }, null, timeout, Timeout.InfiniteTimeSpan) : null;
        var check = TimeSpan.FromSeconds(Math.Min(5, Math.Max(0.5, stallTimeout.TotalSeconds)));
        using var watchdog = stallTimeout > TimeSpan.Zero ? new Timer(_ =>
        {
            if (Environment.TickCount64 - Interlocked.Read(ref _lastOutput) < stallTimeout.TotalMilliseconds || _stalled) return;
            _stalled = true;
            Kill();
        }, null, check, check) : null;

        var exited = p.WaitForExitAsync();
        var abandon = _killed.Task.ContinueWith(_ => Task.Delay(TimeSpan.FromSeconds(30)), TaskScheduler.Default).Unwrap();
        if (await Task.WhenAny(exited, abandon).ConfigureAwait(false) != exited)
            return new Result(-1, _cancelled, _timedOut, _stalled, Abandoned: true); // its job stays: a crash still ends it
        ToolJob.Release(_job);
        _job = IntPtr.Zero;
        // Output normally ends with the process. A child it left behind may keep the pipes open, so don't wait long.
        await Task.WhenAny(Task.WhenAll(readOut, readErr), Task.Delay(TimeSpan.FromSeconds(5))).ConfigureAwait(false);
        return new Result(p.ExitCode, _cancelled, _timedOut, _stalled);
    }

    async Task Pump(StreamReader reader, Action<string> onLine)
    {
        string? line;
        while ((line = await reader.ReadLineAsync().ConfigureAwait(false)) != null)
        {
            Interlocked.Exchange(ref _lastOutput, Environment.TickCount64);
            if (line.Length > 0) onLine(line);
        }
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
            if (_process is { HasExited: false } p)
            {
                p.Kill(entireProcessTree: true);
                _killed.TrySetResult();
            }
        }
        catch (InvalidOperationException) { }
        catch (System.ComponentModel.Win32Exception) { }
    }
}

/// <summary>So that a crash (or a forced quit) of Bromelia leaves no tools behind, every process ProcessRunner starts
/// joins a Job Object of its own with KILL_ON_JOB_CLOSE. Bromelia holds the only handle; when Bromelia ends, however it
/// ends, Windows closes it and ends the process (with what it started). makemkvcon otherwise went on reading the disc
/// into a folder recovery had marked INCOMPLETE. Once the process has ended, the job lets go of what it left running and
/// is closed, as macOS and Linux leave a finished tool's leftovers alone. Windows only; best effort (a process that
/// can't join is left as before).</summary>
public static class ToolJob
{
    /// <summary>Puts a started process in a new job; the job's handle, or zero (not Windows, or Windows refused).</summary>
    public static IntPtr Add(Process p)
    {
        if (!OperatingSystem.IsWindows()) return IntPtr.Zero;
        var job = Create();
        if (job == IntPtr.Zero) return IntPtr.Zero;
        try
        {
            if (Native.AssignProcessToJobObject(job, p.Handle)) return job;
        }
        catch (InvalidOperationException) { }
        catch (Win32Exception) { }
        Native.CloseHandle(job);
        return IntPtr.Zero;
    }

    /// <summary>After the process has ended: clears KILL_ON_JOB_CLOSE (what it left running is left alone) and closes the
    /// job.</summary>
    public static void Release(IntPtr job)
    {
        if (job == IntPtr.Zero) return;
        var info = new Native.ExtendedLimits();
        Native.SetInformationJobObject(job, Native.JobObjectExtendedLimitInformation, ref info, (uint)Marshal.SizeOf<Native.ExtendedLimits>());
        Native.CloseHandle(job);
    }

    /// <summary>Whether <paramref name="p"/> is in <paramref name="job"/>.</summary>
    public static bool Holds(IntPtr job, Process p)
    {
        if (!OperatingSystem.IsWindows() || job == IntPtr.Zero) return false;
        try { return Native.IsProcessInJob(p.Handle, job, out var inJob) && inJob; }
        catch (Exception e) when (e is InvalidOperationException or Win32Exception) { return false; }
    }

    static IntPtr Create()
    {
        var job = Native.CreateJobObjectW(IntPtr.Zero, null);
        if (job == IntPtr.Zero) return IntPtr.Zero;
        var info = new Native.ExtendedLimits();
        info.BasicLimitInformation.LimitFlags = Native.JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (Native.SetInformationJobObject(job, Native.JobObjectExtendedLimitInformation, ref info, (uint)Marshal.SizeOf<Native.ExtendedLimits>())) return job;
        Native.CloseHandle(job);
        return IntPtr.Zero;
    }

    static class Native
    {
        public const int JobObjectExtendedLimitInformation = 9;
        public const uint JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x2000;

        [StructLayout(LayoutKind.Sequential)]
        public struct BasicLimits
        {
            public long PerProcessUserTimeLimit;
            public long PerJobUserTimeLimit;
            public uint LimitFlags;
            public UIntPtr MinimumWorkingSetSize;
            public UIntPtr MaximumWorkingSetSize;
            public uint ActiveProcessLimit;
            public UIntPtr Affinity;
            public uint PriorityClass;
            public uint SchedulingClass;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct IoCounters
        {
            public ulong ReadOperationCount, WriteOperationCount, OtherOperationCount, ReadTransferCount, WriteTransferCount, OtherTransferCount;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct ExtendedLimits
        {
            public BasicLimits BasicLimitInformation;
            public IoCounters IoInfo;
            public UIntPtr ProcessMemoryLimit;
            public UIntPtr JobMemoryLimit;
            public UIntPtr PeakProcessMemoryUsed;
            public UIntPtr PeakJobMemoryUsed;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern IntPtr CreateJobObjectW(IntPtr attributes, string? name);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool SetInformationJobObject(IntPtr job, int infoClass, ref ExtendedLimits info, uint length);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool IsProcessInJob(IntPtr process, IntPtr job, out bool result);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool CloseHandle(IntPtr handle);
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

/// <summary>Queued, waiting and running jobs live only in memory. So that a crash, a power cut or a forced quit never
/// loses one silently, they are also kept in unfinished-&lt;pid&gt;.json. At start, jobs left there by a process that
/// has gone are recorded in the history as interrupted (running: failed) or not started (queued, waiting: cancelled),
/// and a running job's staging folder is made visible as "INCOMPLETE - &lt;id&gt;" with a note.</summary>
public static class UnfinishedJobs
{
    public sealed class Entry
    {
        public Guid Id { get; set; }
        public string Title { get; set; } = "";
        public string DriveName { get; set; } = "";
        public string DiscName { get; set; } = "";
        public RipMode Mode { get; set; }
        public JobState State { get; set; }
        public string? OutputDirectory { get; set; }
        public string LogPath { get; set; } = "";
        public DateTime? StartedAt { get; set; }
    }

    public sealed class Contents
    {
        public int Pid { get; set; }
        /// <summary>Identifies the process, so a file left by an earlier process with the same pid is still recognised as stale.</summary>
        public string Instance { get; set; } = "";
        public List<Entry> Jobs { get; set; } = new();
    }

    public static readonly string Instance = Guid.NewGuid().ToString();

    public static string FileFor(int pid) => Path.Combine(Paths.AppData, $"unfinished-{pid}.json");

    public static void Save(List<Entry> jobs)
    {
        var path = FileFor(Environment.ProcessId);
        if (jobs.Count == 0)
        {
            try { File.Delete(path); } catch (IOException) { } catch (UnauthorizedAccessException) { }
            return;
        }
        ConfigStore.SaveRaw(path, JsonSerializer.Serialize(new Contents { Pid = Environment.ProcessId, Instance = Instance, Jobs = jobs }, ConfigJson.Options));
    }

    /// <summary>History records for the jobs left by processes that have gone; their files are removed.</summary>
    public static List<HistoryRecord> Recover()
    {
        var records = new List<HistoryRecord>();
        List<string> files;
        try { files = Directory.EnumerateFiles(Paths.AppData, "unfinished-*.json").OrderBy(p => p, StringComparer.Ordinal).ToList(); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return records; } // no data folder yet
        foreach (var path in files)
        {
            Contents? c;
            try { c = JsonSerializer.Deserialize<Contents>(File.ReadAllText(path), ConfigJson.Options); }
            catch (Exception) { c = null; }
            if (c != null)
            {
                // Still in use by another Bromelia.
                if (c.Instance == Instance || (c.Pid != Environment.ProcessId && IsAlive(c.Pid))) continue;
                records.AddRange(c.Jobs.Select(Record));
            }
            try { File.Delete(path); } catch (IOException) { } catch (UnauthorizedAccessException) { }
        }
        return records;
    }

    public static bool IsAlive(int pid)
    {
        if (pid <= 0) return false;
        try
        {
            using var p = System.Diagnostics.Process.GetProcessById(pid);
            return !p.HasExited;
        }
        catch (ArgumentException) { return false; }
        catch (InvalidOperationException) { return false; }
    }

    static HistoryRecord Record(Entry e)
    {
        bool running = e.State == JobState.Running;
        var kept = running && !string.IsNullOrEmpty(e.OutputDirectory) ? RecoverStaging(e.OutputDirectory, e.Id, e.LogPath) : null;
        return new HistoryRecord
        {
            Id = e.Id, Title = e.Title, DriveName = e.DriveName, DiscName = e.DiscName, Mode = e.Mode,
            State = running ? JobState.Failed : JobState.Cancelled, StartedAt = e.StartedAt, FinishedAt = DateTime.Now,
            OutputDirectory = kept, LogPath = e.LogPath, Errors = running ? 1 : 0,
            ErrorMessage = !running ? "Not started: Bromelia stopped while this job was waiting"
                : kept != null ? $"Interrupted: Bromelia stopped while this job was running; its files were kept in {kept}"
                : "Interrupted: Bromelia stopped while this job was running; nothing was saved",
        };
    }

    /// <summary>The staging folder of a job that was running: made visible with a note, or removed when nothing was saved.
    /// Returns where its files are now.</summary>
    public static string? RecoverStaging(string outputDir, Guid id, string logPath)
    {
        var shortId = id.ToString("N")[..8];
        var stage = Path.Combine(outputDir, JobRunner.StagingPrefix + shortId);
        if (!Directory.Exists(stage)) return null;
        try
        {
            if (JobRunner.VisibleItems(stage).Count == 0)
            {
                Directory.Delete(stage, true); // only temporary (hidden) files
                // The folder made for the job, if nothing else is in it.
                if (!Directory.EnumerateFileSystemEntries(outputDir).Any()) Directory.Delete(outputDir);
                return null;
            }
            var dest = Paths.UniquePath(Path.Combine(outputDir, $"INCOMPLETE - {shortId}"));
            Directory.Move(stage, dest);
            try { new DirectoryInfo(dest).Attributes &= ~FileAttributes.Hidden; } catch (IOException) { }
            File.WriteAllText(Path.Combine(dest, "INCOMPLETE.txt"),
                $"Bromelia job {id}: interrupted.\n" +
                "Bromelia stopped (it quit, crashed or lost power) while this job was running.\n" +
                "These files are NOT a finished archive. Rip the disc again.\n\n" +
                $"Log up to the interruption: {logPath}\n");
            return dest;
        }
        catch (IOException) { return Directory.Exists(stage) ? stage : null; }
        catch (UnauthorizedAccessException) { return Directory.Exists(stage) ? stage : null; }
    }
}

/// <summary>Only one Bromelia process at a time rips inserted discs and runs the scheduled archive check: the one that holds
/// automation.lock in the data folder open (the app, or bromelia-daemon); automation.owner says which. The other one asks
/// again every minute, so it takes over when the first one quits.</summary>
public sealed class AutomationLock : IDisposable
{
    readonly string _path;
    FileStream? _file;

    public AutomationLock(string? path = null) => _path = path ?? Path.Combine(Paths.AppData, "automation.lock");

    public bool IsHeld => _file != null;

    string OwnerPath => Path.ChangeExtension(_path, ".owner");

    /// <summary>Takes the lock unless another process has it; <paramref name="who"/> is written into automation.owner.</summary>
    public bool Acquire(string who)
    {
        if (_file != null) return true;
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(_path)!);
            _file = new FileStream(_path, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return false; }
        try { File.WriteAllText(OwnerPath, $"{who} {Environment.ProcessId}\n"); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        return true;
    }

    /// <summary>Who holds the lock ("the daemon 1234"), or "".</summary>
    public string Holder
    {
        get
        {
            try { return File.ReadAllText(OwnerPath).Trim(); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return ""; }
        }
    }

    public void Dispose()
    {
        _file?.Dispose();
        _file = null;
    }
}
