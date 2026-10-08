using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Channels;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Adapters.Windows;

/// <summary>Starts processes in a Job Object and watches them (plan §10.3): stalls, stopping, abandonment 30 s after
/// the kill, output read for 5 s after exit (longer only while lines wait for a reader that is still taking them), a
/// transcript of every line. Lines are cut at 64 KiB
/// (<see cref="LineSplitter"/>) and at most <see cref="QueuedLines"/>, or <see cref="QueuedBytes"/> of text, wait to be
/// read: then the reading waits too, so the tool waits to write (nothing is lost) and that wait doesn't count as
/// silence.
///
/// Windows has no signals. A stop (either policy, or a stall) sends CTRL_BREAK to the tool's console (<see
/// cref="ConsoleBreak"/>) so it can close its files, and ends the whole job (TerminateJobObject) 5 s later, or as soon as
/// the tool has exited (whatever it left behind goes too). A tool that exits on the break reports its own exit status;
/// one the job ends reports -1. The job also has KILL_ON_JOB_CLOSE until the process has ended, so a crashed engine leaves no tools
/// behind. A process the tool leaves running after a normal exit is left alone, as on macOS and Linux.</summary>
public sealed class PlatformProcessLauncher : IProcessLauncher
{
    /// <summary>How many lines may wait to be read before the reading waits.</summary>
    public const int QueuedLines = 10000;
    /// <summary>How much text (UTF-8) may wait to be read before the reading waits: long lines for a reader that has
    /// stopped can't hold more than about 16 MiB.</summary>
    public const int QueuedBytes = 16 * 1024 * 1024;

    public IRunningProcess Start(ProcessSpec spec) => Running.Start(spec);

    /// <summary>The program and arguments actually run: the spec's interpreter first, else one chosen from the
    /// script's extension (.ps1: PowerShell, .bat and .cmd: cmd.exe, .py: the py launcher).</summary>
    public static CommandLine Invocation(ProcessSpec spec)
    {
        var exe = spec.Executable;
        IEnumerable<string> Then(params string[] first) => first.Concat(spec.Arguments);
        if (!string.IsNullOrWhiteSpace(spec.Interpreter))
            return new CommandLine(spec.Interpreter!.Trim(), Then(exe).ToList());
        switch (Path.GetExtension(exe).ToLowerInvariant())
        {
            case ".ps1":
                return new CommandLine("powershell.exe", Then("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", exe).ToList());
            case ".bat":
            case ".cmd":
                return new CommandLine(Environment.GetEnvironmentVariable("ComSpec") is { Length: > 0 } c ? c : "cmd.exe", Then("/c", exe).ToList());
            case ".py":
                return new CommandLine("py.exe", Then(exe).ToList());
        }
        return new CommandLine(exe, spec.Arguments.ToList());
    }

    sealed class Running : IRunningProcess
    {
        static readonly TimeSpan Tick = TimeSpan.FromMilliseconds(250);

        readonly Process _process;
        /// <summary>The Job Object, until <see cref="Finish"/> closes it (guarded by <see cref="_gate"/>).</summary>
        IntPtr _job;
        readonly ProcessSpec _spec;
        readonly Channel<OutputLine> _lines = Channel.CreateBounded<OutputLine>(new BoundedChannelOptions(QueuedLines)
        {
            SingleReader = true,
            FullMode = BoundedChannelFullMode.Wait,
        });
        /// <summary>Pumps waiting for room in <see cref="_lines"/>: the tool isn't silent, nobody is reading.</summary>
        int _waitingForRoom;
        /// <summary>When the reader last took a line.</summary>
        long _lastTaken = Environment.TickCount64;
        /// <summary>The text of the lines in <see cref="_lines"/>; a pump waits while it is <see cref="QueuedBytes"/>.</summary>
        long _queuedBytes;
        /// <summary>Released when the reader takes a line: room, maybe.</summary>
        readonly SemaphoreSlim _taken = new(0, int.MaxValue);
        volatile bool _ended;
        readonly object _gate = new();
        readonly TaskCompletionSource _killed = new(TaskCreationOptions.RunContinuationsAsynchronously);
        readonly Transcript? _transcript;
        readonly Task<ProcessExit> _exit;
        Timer? _watchdog;
        long _lastOutput = Environment.TickCount64;
        StopReason? _reason;
        Duration? _stalled;

        Running(ProcessSpec spec, Process process, IntPtr job, Transcript? transcript)
        {
            _spec = spec;
            _process = process;
            _job = job;
            _transcript = transcript;
            var pumps = new[] { Pump(process.StandardOutput.BaseStream, OutputSource.Stdout), Pump(process.StandardError.BaseStream, OutputSource.Stderr) };
            if (spec.StallTimeout is not null) _watchdog = new Timer(_ => Watch(), null, Tick, Tick);
            _exit = Finish(pumps);
        }

        public static Running Start(ProcessSpec spec)
        {
            var cmd = Invocation(spec);
            var transcript = spec.Transcript is { Length: > 0 } t ? new Transcript(t) : null;
            transcript?.Write($"==== {Instant.Format(Now())} $ {string.Join(" ", new[] { cmd.Executable }.Concat(cmd.Arguments).Select(ArgumentSplitter.Quote))}");
            try
            {
                if (spec.WorkingDirectory is { Length: > 0 } wd && !Directory.Exists(wd))
                    throw new DirectoryNotFoundException($"the working folder {wd} doesn't exist");
                var psi = new ProcessStartInfo(cmd.Executable)
                {
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    RedirectStandardInput = true,
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    StandardOutputEncoding = new UTF8Encoding(false),
                    StandardErrorEncoding = new UTF8Encoding(false),
                };
                foreach (var a in cmd.Arguments) psi.ArgumentList.Add(a);
                foreach (var kv in spec.Environment) psi.Environment[kv.Key] = kv.Value;
                if (spec.WorkingDirectory is { Length: > 0 } dir) psi.WorkingDirectory = dir;

                var job = Native.CreateKillOnCloseJob();
                var p = new Process { StartInfo = psi };
                try
                {
                    p.Start();
                }
                catch
                {
                    Native.CloseHandle(job);
                    throw;
                }
                // A child the process starts in the moment before this call escapes the job; Process.Start can't
                // start it suspended. Stop then still ends the process tree (see Kill).
                if (job != IntPtr.Zero && !Native.AssignProcessToJobObject(job, p.Handle))
                {
                    Native.CloseHandle(job);
                    job = IntPtr.Zero;
                }
                p.StandardInput.Close();
                return new Running(spec, p, job, transcript);
            }
            catch (Exception e) when (e is Win32Exception or IOException or InvalidOperationException or UnauthorizedAccessException)
            {
                transcript?.Write($"==== {Instant.Format(Now())} could not start: {e.Message}");
                transcript?.Dispose();
                throw new BroFailure(new BroMessage(MessageCode.ProcessCouldNotStart, Severity.Error,
                    ("tool", JsonValue.Of(Path.GetFileName(cmd.Executable))), ("reason", JsonValue.Of(e.Message))).ToError());
            }
        }

        public async IAsyncEnumerable<OutputLine> Lines()
        {
            await foreach (var line in _lines.Reader.ReadAllAsync().ConfigureAwait(false))
            {
                Interlocked.Exchange(ref _lastTaken, Environment.TickCount64);
                Interlocked.Add(ref _queuedBytes, -Encoding.UTF8.GetByteCount(line.Text));
                if (_taken.CurrentCount == 0) _taken.Release();
                yield return line;
            }
        }

        public Task<ProcessExit> Wait() => _exit;

        public BroMessage? TranscriptProblem() => _transcript?.Problem;

        /// <summary>How long a tool has after CTRL_BREAK before the job is ended, as macOS and Linux wait after a signal.</summary>
        static readonly TimeSpan Grace = TimeSpan.FromSeconds(5);

        public void Stop(StopReason reason)
        {
            lock (_gate)
            {
                if (_reason is not null) return;
                _reason = reason;
            }
            Escalate();
        }

        /// <summary>CTRL_BREAK, then the job ends after the grace period or as soon as the tool has exited.</summary>
        void Escalate()
        {
            try
            {
                if (!_process.HasExited) ConsoleBreak.Send(_process.Id);
            }
            catch (InvalidOperationException) { }
            _ = Task.Run(async () =>
            {
                await Task.WhenAny(_process.WaitForExitAsync(), Task.Delay(Grace)).ConfigureAwait(false);
                Kill();
            });
        }

        static Instant Now() => new(DateTimeOffset.UtcNow.ToUnixTimeMilliseconds());

        void Watch()
        {
            var limit = _spec.StallTimeout!.Value;
            if (Volatile.Read(ref _waitingForRoom) > 0) return;
            var quiet = (Environment.TickCount64 - Interlocked.Read(ref _lastOutput)) / 1000.0;
            if (quiet < limit.Seconds) return;
            lock (_gate)
            {
                if (_reason is not null) return;
                _reason = StopReason.Stalled;
                _stalled = new Duration(Math.Round(quiet, 3));
            }
            Escalate();
        }

        /// <summary>Ends the job: the tool if it is still running, and whatever it left behind.</summary>
        void Kill()
        {
            try
            {
                bool ended;
                lock (_gate) ended = _job != IntPtr.Zero && Native.TerminateJobObject(_job, unchecked((uint)-1));
                if (!ended && !_process.HasExited) _process.Kill(entireProcessTree: true);
            }
            catch (InvalidOperationException) { }
            catch (Win32Exception) { }
            _killed.TrySetResult();
        }

        async Task Pump(Stream stream, OutputSource source)
        {
            var splitter = new LineSplitter();
            var buffer = new byte[65536];
            int n;
            while ((n = await stream.ReadAsync(buffer).ConfigureAwait(false)) > 0)
            {
                Interlocked.Exchange(ref _lastOutput, Environment.TickCount64);
                foreach (var line in splitter.Feed(buffer.AsSpan(0, n))) await Emit(line, source).ConfigureAwait(false);
            }
            if (splitter.Finish() is { } last) await Emit(last, source).ConfigureAwait(false);
        }

        /// <summary>Into the transcript, then the queue; when the queue is full this waits for room (a line after the
        /// end is dropped).</summary>
        async Task Emit(string line, OutputSource source)
        {
            _transcript?.Write(line);
            var item = new OutputLine(source, line, Now());
            if (Interlocked.Read(ref _queuedBytes) >= QueuedBytes)
            {
                Interlocked.Increment(ref _waitingForRoom);
                try
                {
                    while (Interlocked.Read(ref _queuedBytes) >= QueuedBytes && !_ended) await _taken.WaitAsync(Tick).ConfigureAwait(false);
                }
                finally
                {
                    Interlocked.Exchange(ref _lastOutput, Environment.TickCount64);
                    Interlocked.Decrement(ref _waitingForRoom);
                }
                if (_ended) return;
            }
            Interlocked.Add(ref _queuedBytes, Encoding.UTF8.GetByteCount(line));
            if (_lines.Writer.TryWrite(item)) return;
            Interlocked.Increment(ref _waitingForRoom);
            try { await _lines.Writer.WriteAsync(item).ConfigureAwait(false); }
            catch (ChannelClosedException) { }
            finally
            {
                Interlocked.Exchange(ref _lastOutput, Environment.TickCount64);
                Interlocked.Decrement(ref _waitingForRoom);
            }
        }

        async Task<ProcessExit> Finish(Task[] pumps)
        {
            var exited = _process.WaitForExitAsync();
            var abandon = _killed.Task.ContinueWith(_ => Task.Delay(TimeSpan.FromSeconds(30)), TaskScheduler.Default).Unwrap();
            var abandoned = await Task.WhenAny(exited, abandon).ConfigureAwait(false) != exited;
            // Output normally ends with the process. A child it left behind may keep the pipes open: stop reading 5 s after
            // the exit, unless lines are still waiting for a reader that keeps taking them (its backlog isn't dropped).
            // Queued lines count too: a pump between two waits for room isn't counted, and ending then would drop what
            // it hasn't read yet.
            if (!abandoned)
            {
                var exitedAt = Environment.TickCount64;
                var all = Task.WhenAll(pumps);
                bool Reading() => Environment.TickCount64 - exitedAt < 5000
                                  || ((Volatile.Read(ref _waitingForRoom) > 0 || _lines.Reader.Count > 0)
                                      && Environment.TickCount64 - Interlocked.Read(ref _lastTaken) < 5000);
                while (!all.IsCompleted && Reading()) await Task.WhenAny(all, Task.Delay(Tick)).ConfigureAwait(false);
            }
            _watchdog?.Dispose();
            _watchdog = null;
            _ended = true;
            _lines.Writer.TryComplete();
            lock (_gate)
            {
                if (_job != IntPtr.Zero)
                {
                    Native.KeepProcessesOnClose(_job);
                    Native.CloseHandle(_job);
                    _job = IntPtr.Zero;
                }
            }
            ProcessExit exit;
            lock (_gate)
                exit = new ProcessExit(abandoned ? -1 : _process.ExitCode, null, _stalled, abandoned,
                    _reason is StopReason.Cancelled or StopReason.Shutdown);
            if (_transcript is not null)
            {
                _transcript.Write($"==== {Instant.Format(Now())} {Transcript.Ending(exit)}");
                _transcript.Dispose();
            }
            return exit;
        }
    }

    /// <summary>The transcript file: appended to, one line at a time, from both output pumps. A line that comes after
    /// the end (a child left behind kept a pipe open) is dropped.</summary>
    sealed class Transcript : IDisposable
    {
        readonly string _path;
        readonly object _gate = new();
        readonly StreamWriter? _writer;
        bool _closed;
        BroMessage? _problem;

        public Transcript(string path)
        {
            _path = path;
            try
            {
                if (Path.GetDirectoryName(path) is { Length: > 0 } dir) Directory.CreateDirectory(dir);
                _writer = new StreamWriter(new FileStream(path, FileMode.Append, FileAccess.Write, FileShare.Read), new UTF8Encoding(false)) { NewLine = "\n", AutoFlush = true };
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                _writer = null;
                Fail(e.Message);
            }
        }

        /// <summary>process.noTranscript for the first failure; null when every line was written.</summary>
        public BroMessage? Problem
        {
            get { lock (_gate) return _problem; }
        }

        void Fail(string reason)
        {
            lock (_gate)
                _problem ??= new BroMessage(MessageCode.ProcessNoTranscript, Severity.Warning, ("path", JsonValue.Of(_path)), ("reason", JsonValue.Of(reason)));
        }

        /// <summary><c>exit status 0</c>, <c>signal 15</c> or <c>abandoned</c>, then <c>, stalled</c> and
        /// <c>, cancelled</c> when they apply.</summary>
        public static string Ending(ProcessExit exit)
        {
            var s = exit.Abandoned ? "abandoned" : exit.Signal is { } sig ? $"signal {sig}" : $"exit status {exit.Status}";
            if (exit.Stalled is not null) s += ", stalled";
            if (exit.Cancelled) s += ", cancelled";
            return s;
        }

        public void Write(string line)
        {
            if (_writer is null) return;
            lock (_writer)
            {
                if (_closed) return;
                try { _writer.WriteLine(line); }
                catch (IOException e) { Fail(e.Message); }
            }
        }

        /// <summary>Closing writes what the file stream still buffers: on a full disk that fails like any line
        /// (process.noTranscript), never the run.</summary>
        public void Dispose()
        {
            if (_writer is null) return;
            lock (_writer)
            {
                _closed = true;
                try { _writer.Dispose(); }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Fail(e.Message); }
            }
        }
    }

    /// <summary>CTRL_BREAK for a tool. The tool runs in a console of its own (CreateNoWindow gives every tool one, even
    /// when the engine has a console), and only a process attached to that console can send it. Attaching the engine
    /// itself doesn't work: the break then ends the engine too, whatever handler it set (found 2026-10-07). So a
    /// short-lived hidden PowerShell attaches to the tool's console and sends the break to everything on it (the tool
    /// and its children); the break ends the helper as well, which is all it is for (about 0.7 s on the test VM).
    /// Best effort: a tool the break doesn't reach, or without PowerShell, is ended with its job after the grace
    /// period.</summary>
    internal static class ConsoleBreak
    {
        public static void Send(int pid)
        {
            var script = "Add-Type -Name C -Namespace BromeliaBreak -MemberDefinition '" +
                         "[DllImport(\"kernel32.dll\")] public static extern bool FreeConsole(); " +
                         "[DllImport(\"kernel32.dll\")] public static extern bool AttachConsole(uint p); " +
                         "[DllImport(\"kernel32.dll\")] public static extern bool GenerateConsoleCtrlEvent(uint e, uint g);'; " +
                         $"[void][BromeliaBreak.C]::FreeConsole(); if ([BromeliaBreak.C]::AttachConsole({pid})) {{ [void][BromeliaBreak.C]::GenerateConsoleCtrlEvent(1, 0) }}";
            try
            {
                var psi = new ProcessStartInfo("powershell.exe") { UseShellExecute = false, CreateNoWindow = true };
                foreach (var a in new[] { "-NoProfile", "-NonInteractive", "-Command", script }) psi.ArgumentList.Add(a);
                Process.Start(psi)?.Dispose();
            }
            catch (Win32Exception) { }
        }
    }

    static class Native
    {
        const int JobObjectExtendedLimitInformation = 9;
        const uint JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x2000;

        [StructLayout(LayoutKind.Sequential)]
        struct BasicLimits
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
        struct IoCounters
        {
            public ulong ReadOperationCount, WriteOperationCount, OtherOperationCount, ReadTransferCount, WriteTransferCount, OtherTransferCount;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct ExtendedLimits
        {
            public BasicLimits BasicLimitInformation;
            public IoCounters IoInfo;
            public UIntPtr ProcessMemoryLimit;
            public UIntPtr JobMemoryLimit;
            public UIntPtr PeakProcessMemoryUsed;
            public UIntPtr PeakJobMemoryUsed;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern IntPtr CreateJobObjectW(IntPtr attributes, string? name);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool SetInformationJobObject(IntPtr job, int infoClass, ref ExtendedLimits info, uint length);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool TerminateJobObject(IntPtr job, uint exitCode);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool CloseHandle(IntPtr handle);

        /// <summary>A job whose processes end when its last handle closes (the engine crashing included); zero
        /// when Windows refuses one.</summary>
        public static IntPtr CreateKillOnCloseJob()
        {
            var job = CreateJobObjectW(IntPtr.Zero, null);
            if (job == IntPtr.Zero) return IntPtr.Zero;
            var info = new ExtendedLimits();
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (SetInformationJobObject(job, JobObjectExtendedLimitInformation, ref info, (uint)Marshal.SizeOf<ExtendedLimits>())) return job;
            CloseHandle(job);
            return IntPtr.Zero;
        }

        /// <summary>Clears KILL_ON_JOB_CLOSE, so closing the handle leaves what is still running alone.</summary>
        public static void KeepProcessesOnClose(IntPtr job)
        {
            var info = new ExtendedLimits();
            SetInformationJobObject(job, JobObjectExtendedLimitInformation, ref info, (uint)Marshal.SizeOf<ExtendedLimits>());
        }
    }
}
