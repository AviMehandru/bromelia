using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.TestKit;

/// <summary>A launcher that plays a script instead of running anything (plan §17's test kit): the lines, one at a time
/// as they are asked for; then the exit status, after creating files in the destination. A stop ends it at once with
/// status -1 and signal 15. It records every spec it was given. The files it creates go to real disk, or through
/// <see cref="Files"/> when it has one (the test root's memory file system).</summary>
public sealed class ScriptedProcessLauncher : IProcessLauncher
{
    public readonly List<ProcessSpec> Started = new();
    /// <summary>Where it creates its output files (null: real disk).</summary>
    public readonly IFileSystem? Files;
    /// <summary>The lines for each spec (null, or a null answer: <see cref="Lines"/>), e.g. a drive scan against a rip.</summary>
    public Func<ProcessSpec, IReadOnlyList<string>?>? LinesFor;
    /// <summary>A spec this answers true for runs until it is stopped after its lines (a rip in progress).</summary>
    public Func<ProcessSpec, bool>? RunsUntilStopped;
    /// <summary>How long a stopped process takes to end (a process slow to die); real time.</summary>
    public TimeSpan StopTakes = TimeSpan.Zero;
    int _held;
    int _running;
    public IReadOnlyList<string> Lines = Array.Empty<string>();
    /// <summary>The indexes of the lines that come on stderr (the others on stdout).</summary>
    public ISet<int> Stderr = new HashSet<int>();
    public int ExitCode;
    public string? WriteFileIn;
    /// <summary>The files written in <see cref="WriteFileIn"/>: a name with / makes its folders; "" writes
    /// <see cref="WriteFileIn"/> itself as a file.</summary>
    public IReadOnlyList<string> WriteFileNames = new[] { "title_t00.mkv" };
    /// <summary>After the lines: end as stopped for silence (status -1, signal 15, stalled for the spec's timeout).</summary>
    public bool Stalls;
    public Action<int>? AfterLine;
    /// <summary>When it ends normally: create this many parts from the -o argument's %03d pattern (mkvmerge --split).</summary>
    public int SplitParts;
    /// <summary>When it ends normally: write this text to the file named by argument Index (mkvextract's output).</summary>
    public (int Index, string Text)? WriteArgument;
    public Script? Last;
    /// <summary>The process reports process.noTranscript for the spec's transcript.</summary>
    public bool TranscriptFails;

    public ScriptedProcessLauncher(IFileSystem? files = null) { Files = files; }

    public IRunningProcess Start(ProcessSpec spec)
    {
        var script = new Script(this, spec);
        Interlocked.Increment(ref _running);
        script.Wait().ContinueWith(_ => Interlocked.Decrement(ref _running), TaskContinuationOptions.ExecuteSynchronously);
        lock (Started)
        {
            Started.Add(spec);
            Last = script;
        }
        return script;
    }

    /// <summary>The processes waiting to be stopped after their lines (<see cref="RunsUntilStopped"/>): each is a step a
    /// harness knows is parked.</summary>
    public int Held() => Volatile.Read(ref _held);

    /// <summary>The processes that haven't ended yet.</summary>
    public int Running() => Volatile.Read(ref _running);

    /// <summary>The specs started so far (any thread).</summary>
    public IReadOnlyList<ProcessSpec> Specs()
    {
        lock (Started) return Started.ToList();
    }

    void WriteText(string path, string text)
    {
        if (Files is { } fs)
        {
            fs.CreateDirectory(path[..path.LastIndexOf('/')], parentsMustExist: false); // the test root's paths use '/' everywhere
            fs.WriteAtomically(path, System.Text.Encoding.UTF8.GetBytes(text), 0x1A4);
            return;
        }
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, text);
    }

    public sealed class Script : IRunningProcess
    {
        readonly ScriptedProcessLauncher _owner;
        readonly ProcessSpec _spec;
        readonly TaskCompletionSource<ProcessExit> _exit = new(TaskCreationOptions.RunContinuationsAsynchronously);
        readonly TaskCompletionSource _stop = new(TaskCreationOptions.RunContinuationsAsynchronously);
        StopReason? _stopped;
        public int Handed;

        public Script(ScriptedProcessLauncher owner, ProcessSpec spec)
        {
            _owner = owner;
            _spec = spec;
        }

        public StopReason? StoppedBy => _stopped;

        public async IAsyncEnumerable<OutputLine> Lines()
        {
            foreach (var text in _owner.LinesFor?.Invoke(_spec) ?? _owner.Lines)
            {
                // A real hop to the thread pool. Not Task.Yield: that posts to xUnit's synchronization context, whose
                // few threads the tests block with GetResult (a deadlock on a two-core machine).
                await Task.Run(() => { }).ConfigureAwait(false);
                if (_stopped != null) break;
                Handed++;
                yield return new OutputLine(_owner.Stderr.Contains(Handed - 1) ? OutputSource.Stderr : OutputSource.Stdout, text, new Instant(0));
                _owner.AfterLine?.Invoke(Handed);
            }
            if (_stopped is null && _owner.RunsUntilStopped?.Invoke(_spec) == true)
            {
                Interlocked.Increment(ref _owner._held);
                await _stop.Task.ConfigureAwait(false);
                Interlocked.Decrement(ref _owner._held);
            }
            if (_stopped is null && _owner.Stalls)
                _exit.TrySetResult(new ProcessExit(-1, 15, _spec.StallTimeout ?? new Duration(0)));
            else if (_stopped is { } reason)
                _exit.TrySetResult(new ProcessExit(-1, 15, null, false, reason is StopReason.Cancelled or StopReason.Shutdown));
            else
            {
                if (_owner.WriteFileIn is { } dir)
                    foreach (var name in _owner.WriteFileNames)
                    {
                        var path = name.Length == 0 ? dir : _owner.Files != null ? dir + "/" + name : Path.Combine(dir, name);
                        _owner.WriteText(path, "");
                    }
                if (_owner.SplitParts > 0)
                {
                    var pattern = _spec.Arguments[_spec.Arguments.ToList().IndexOf("-o") + 1];
                    for (int i = 1; i <= _owner.SplitParts; i++) _owner.WriteText(pattern.Replace("%03d", i.ToString("D3")), "");
                }
                if (_owner.WriteArgument is { } w) _owner.WriteText(_spec.Arguments[w.Index], w.Text);
                _exit.TrySetResult(new ProcessExit(_owner.ExitCode));
            }
        }

        public Task<ProcessExit> Wait() => _exit.Task;

        public BroMessage? TranscriptProblem() => _owner.TranscriptFails
            ? new BroMessage(MessageCode.ProcessNoTranscript, Severity.Warning, ("path", JsonValue.Of(_spec.Transcript ?? "")), ("reason", JsonValue.Of("No space left on device")))
            : null;

        /// <summary>As a real process: it ends when stopped, whether or not anyone reads the rest of its lines.</summary>
        public void Stop(StopReason reason)
        {
            _stopped ??= reason;
            var exit = new ProcessExit(-1, 15, null, false, _stopped is StopReason.Cancelled or StopReason.Shutdown);
            if (_owner.StopTakes > TimeSpan.Zero)
                Task.Delay(_owner.StopTakes).ContinueWith(_ =>
                {
                    _exit.TrySetResult(exit);
                    _stop.TrySetResult();
                });
            else
            {
                _exit.TrySetResult(exit);
                _stop.TrySetResult();
            }
        }
    }
}
