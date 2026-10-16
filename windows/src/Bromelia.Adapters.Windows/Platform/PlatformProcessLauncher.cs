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
/// the kill, output read for 5 s after exit (then what the pipes hold is still taken, so nothing written before the exit
/// is lost, however slow the reader), a transcript of every line. Lines are cut at 64 KiB
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
                return new CommandLine(SystemPrograms.PowerShell, Then("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", exe).ToList());
            case ".bat":
            case ".cmd":
                return new CommandLine(Environment.GetEnvironmentVariable("ComSpec") is { Length: > 0 } c ? c : SystemPrograms.Cmd, Then("/c", exe).ToList());
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
        /// <summary>The lines waiting to be read; a pump waits while there are <see cref="QueuedLines"/> of them, unless
        /// the pumps are draining. (Not SingleReader: that reader can't count.)</summary>
        readonly Channel<OutputLine> _lines = Channel.CreateUnbounded<OutputLine>();
        /// <summary>Pumps waiting for room in <see cref="_lines"/>: the tool isn't silent, nobody is reading.</summary>
        int _waitingForRoom;
        /// <summary>The text of the lines in <see cref="_lines"/>; a pump waits while it is <see cref="QueuedBytes"/>.</summary>
        long _queuedBytes;
        /// <summary>Released when the reader takes a line: room, maybe.</summary>
        readonly SemaphoreSlim _taken = new(0, int.MaxValue);
        volatile bool _ended;
        /// <summary>The pumps take what the pipes hold now, without waiting for room, then stop.</summary>
        volatile bool _draining;
        /// <summary>The pumps still reading (guarded by <see cref="_gate"/>).</summary>
        readonly List<PumpThread> _pumps = new();
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
            var pumps = new[] { Pump(PipeOf(process.StandardOutput.BaseStream), OutputSource.Stdout), Pump(PipeOf(process.StandardError.BaseStream), OutputSource.Stderr) };
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

        /// <summary>The pipe under a redirected stream, read directly: only <see cref="Pump"/> reads it.</summary>
        static SafeHandle PipeOf(Stream stream) => stream switch
        {
            FileStream file => file.SafeFileHandle,
            System.IO.Pipes.PipeStream pipe => pipe.SafePipeHandle,
            _ => throw new InvalidOperationException($"a redirected stream that isn't a pipe: {stream.GetType()}"),
        };

        /// <summary>A pump's thread, for <see cref="Native.CancelSynchronousIo"/> while it is in ReadFile.</summary>
        sealed class PumpThread
        {
            public IntPtr Handle;
            public volatile bool InRead;
        }

        /// <summary>Reads the pipe on a thread of its own.</summary>
        Task Pump(SafeHandle pipe, OutputSource source)
        {
            var done = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            new Thread(() =>
            {
                try
                {
                    Read(pipe, source);
                    done.SetResult();
                }
                catch (Exception e)
                {
                    done.SetException(e);
                }
            }) { IsBackground = true, Name = $"bro-process-{source}" }.Start();
            return done.Task;
        }

        /// <summary>Reads the pipe to its end, or until the pumps drain: then it reads what the pipe holds at that moment,
        /// and no more (a child left behind may go on writing), and stops. Everything written before the exit is by then in
        /// the pipe or already read, so none of it is lost however late this pump runs. A drain cancels a ReadFile that is
        /// still waiting (CancelSynchronousIo): one the kernel has already filled returns its bytes, one still waiting
        /// returns none, so the cancel loses nothing.</summary>
        void Read(SafeHandle pipe, OutputSource source)
        {
            var splitter = new LineSplitter();
            var buffer = new byte[65536];
            var me = new PumpThread { Handle = Native.CurrentThreadForCancel() };
            lock (_gate) _pumps.Add(me);
            void Deliver(int n)
            {
                Interlocked.Exchange(ref _lastOutput, Environment.TickCount64);
                foreach (var line in splitter.Feed(buffer.AsSpan(0, n))) Emit(line, source);
            }
            try
            {
                var atEnd = false;
                while (!_draining)
                {
                    me.InRead = true;
                    var read = Native.ReadPipe(pipe, buffer, buffer.Length, out var n, out var error);
                    me.InRead = false;
                    if (read)
                        Deliver(n);
                    else if (error != Native.ErrorOperationAborted)
                    {
                        atEnd = true;
                        break;
                    }
                }
                for (var left = atEnd ? 0 : Native.BytesInPipe(pipe); left > 0;)
                {
                    if (!Native.ReadPipe(pipe, buffer, Math.Min(left, buffer.Length), out var n, out _) || n == 0) break;
                    left -= n;
                    Deliver(n);
                }
                if (splitter.Finish() is { } last) Emit(last, source);
            }
            finally
            {
                lock (_gate)
                {
                    _pumps.Remove(me);
                    if (me.Handle != IntPtr.Zero) Native.CloseHandle(me.Handle);
                    me.Handle = IntPtr.Zero;
                }
            }
        }

        /// <summary>Cancels the ReadFile each pump is waiting in, if any.</summary>
        void CancelReads()
        {
            lock (_gate)
                foreach (var pump in _pumps)
                    if (pump.InRead && pump.Handle != IntPtr.Zero) Native.CancelSynchronousIo(pump.Handle);
        }

        bool Full() => _lines.Reader.Count >= QueuedLines || Interlocked.Read(ref _queuedBytes) >= QueuedBytes;

        /// <summary>Into the transcript, then the queue; when the queue is full this waits for room, unless the pumps are
        /// draining (a line after the end is dropped).</summary>
        void Emit(string line, OutputSource source)
        {
            _transcript?.Write(line);
            var item = new OutputLine(source, line, Now());
            if (Full() && !_draining)
            {
                Interlocked.Increment(ref _waitingForRoom);
                try
                {
                    while (Full() && !_ended && !_draining) _taken.Wait(Tick);
                }
                finally
                {
                    Interlocked.Exchange(ref _lastOutput, Environment.TickCount64);
                    Interlocked.Decrement(ref _waitingForRoom);
                }
            }
            if (_ended) return;
            var bytes = Encoding.UTF8.GetByteCount(line);
            Interlocked.Add(ref _queuedBytes, bytes);
            if (!_lines.Writer.TryWrite(item)) Interlocked.Add(ref _queuedBytes, -bytes); // the end came first
        }

        async Task<ProcessExit> Finish(Task[] pumps)
        {
            var exited = _process.WaitForExitAsync();
            var abandon = _killed.Task.ContinueWith(_ => Task.Delay(TimeSpan.FromSeconds(30)), TaskScheduler.Default).Unwrap();
            var abandoned = await Task.WhenAny(exited, abandon).ConfigureAwait(false) != exited;
            // Output normally ends with the process. A child it left behind may keep the pipes open: 5 s after the exit the
            // pumps drain (they take what the pipes hold then, without waiting for room) and stop. The end waits for them,
            // not for a deadline: a pump the system hasn't run yet may still have, in its pipe, lines written before the
            // exit.
            if (!abandoned)
            {
                var all = Task.WhenAll(pumps);
                await Task.WhenAny(all, Task.Delay(TimeSpan.FromSeconds(5))).ConfigureAwait(false);
                _draining = true;
                _taken.Release(); // a pump waiting for room stops waiting
                // Again each tick: a pump may enter ReadFile just after a cancel.
                for (CancelReads(); !all.IsCompleted; CancelReads()) await Task.WhenAny(all, Task.Delay(Tick)).ConfigureAwait(false);
                await all.ConfigureAwait(false);
            }
            else
            {
                _draining = true; // the pumps stop, and a line they still read is dropped
                CancelReads();
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
                var psi = new ProcessStartInfo(SystemPrograms.PowerShell) { UseShellExecute = false, CreateNoWindow = true };
                foreach (var a in new[] { "-NoProfile", "-NonInteractive", "-Command", script }) psi.ArgumentList.Add(a);
                Process.Start(psi)?.Dispose();
            }
            catch (Win32Exception) { }
        }
    }

    /// <summary>Windows' own programs by their full path: a bare name is looked up in the engine's current folder before
    /// System32, so a powershell.exe planted there would run instead.</summary>
    internal static class SystemPrograms
    {
        public static string PowerShell => InSystem32(@"WindowsPowerShell\v1.0\powershell.exe", "powershell.exe");
        public static string Cmd => InSystem32("cmd.exe", "cmd.exe");

        static string InSystem32(string relative, string name) =>
            Environment.SystemDirectory is { Length: > 0 } system && File.Exists(Path.Combine(system, relative)) ? Path.Combine(system, relative) : name;
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

        public const int ErrorOperationAborted = 995;
        const uint THREAD_TERMINATE = 0x0001;

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool PeekNamedPipe(SafeHandle pipe, IntPtr buffer, uint size, IntPtr read, out uint available, IntPtr left);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool ReadFile(SafeHandle file, byte[] buffer, uint size, out uint read, IntPtr overlapped);

        [DllImport("kernel32.dll")]
        static extern uint GetCurrentThreadId();

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr OpenThread(uint access, bool inherit, uint threadId);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool CancelSynchronousIo(IntPtr thread);

        /// <summary>The bytes waiting in a pipe; 0 once it has ended.</summary>
        public static int BytesInPipe(SafeHandle pipe) =>
            PeekNamedPipe(pipe, IntPtr.Zero, 0, IntPtr.Zero, out var available, IntPtr.Zero) ? (int)Math.Min(available, int.MaxValue) : 0;

        /// <summary>A blocking read of up to <paramref name="size"/> bytes; false with the Win32 error at the end
        /// (ERROR_BROKEN_PIPE) or when cancelled (<see cref="ErrorOperationAborted"/>).</summary>
        public static bool ReadPipe(SafeHandle pipe, byte[] buffer, int size, out int read, out int error)
        {
            var ok = ReadFile(pipe, buffer, (uint)size, out var n, IntPtr.Zero);
            error = ok ? 0 : Marshal.GetLastWin32Error();
            read = (int)n;
            return ok;
        }

        /// <summary>A handle to the calling thread that CancelSynchronousIo accepts; zero if Windows refuses one.</summary>
        public static IntPtr CurrentThreadForCancel() => OpenThread(THREAD_TERMINATE, false, GetCurrentThreadId());

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
