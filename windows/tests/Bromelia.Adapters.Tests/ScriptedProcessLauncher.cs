using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Adapters.Tests;

/// <summary>A launcher that plays a script instead of running anything (plan §17's test kit): the lines, one at a time
/// as they are asked for; then the exit status, after creating a file in the destination. A stop ends it at once with
/// status -1 and signal 15. It records every spec it was given.</summary>
internal sealed class ScriptedProcessLauncher : IProcessLauncher
{
    public readonly List<ProcessSpec> Started = new();
    public IReadOnlyList<string> Lines = Array.Empty<string>();
    /// <summary>The indexes of the lines that come on stderr (the others on stdout).</summary>
    public ISet<int> Stderr = new HashSet<int>();
    public int ExitCode;
    public string? WriteFileIn;
    /// <summary>The name of the file written in <see cref="WriteFileIn"/>.</summary>
    public string WriteFileName = "title_t00.mkv";
    /// <summary>After the lines: end as stopped for silence (status -1, signal 15, stalled for the spec's timeout).</summary>
    public bool Stalls;
    public Action<int>? AfterLine;
    /// <summary>When it ends normally: create this many parts from the -o argument's %03d pattern (mkvmerge --split).</summary>
    public int SplitParts;
    /// <summary>When it ends normally: write this text to the file named by argument Index (mkvextract's output).</summary>
    public (int Index, string Text)? WriteArgument;
    public Script? Last;

    public IRunningProcess Start(ProcessSpec spec)
    {
        Started.Add(spec);
        Last = new Script(this, spec);
        return Last;
    }

    internal sealed class Script : IRunningProcess
    {
        readonly ScriptedProcessLauncher _owner;
        readonly ProcessSpec _spec;
        readonly TaskCompletionSource<ProcessExit> _exit = new(TaskCreationOptions.RunContinuationsAsynchronously);
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
            foreach (var text in _owner.Lines)
            {
                // A real hop to the thread pool. Not Task.Yield: that posts to xUnit's synchronization context, whose
                // few threads the tests block with GetResult (a deadlock on a two-core machine).
                await Task.Run(() => { }).ConfigureAwait(false);
                if (_stopped != null) break;
                Handed++;
                yield return new OutputLine(_owner.Stderr.Contains(Handed - 1) ? OutputSource.Stderr : OutputSource.Stdout, text, new Instant(0));
                _owner.AfterLine?.Invoke(Handed);
            }
            if (_stopped is null && _owner.Stalls)
                _exit.TrySetResult(new ProcessExit(-1, 15, _spec.StallTimeout ?? new Duration(0)));
            else if (_stopped is { } reason)
                _exit.TrySetResult(new ProcessExit(-1, 15, null, false, reason is StopReason.Cancelled or StopReason.Shutdown));
            else
            {
                if (_owner.WriteFileIn is { } dir) File.WriteAllText(Path.Combine(dir, _owner.WriteFileName), "");
                if (_owner.SplitParts > 0)
                {
                    var pattern = _spec.Arguments[_spec.Arguments.ToList().IndexOf("-o") + 1];
                    for (int i = 1; i <= _owner.SplitParts; i++) File.WriteAllText(pattern.Replace("%03d", i.ToString("D3")), "");
                }
                if (_owner.WriteArgument is { } w) File.WriteAllText(_spec.Arguments[w.Index], w.Text);
                _exit.TrySetResult(new ProcessExit(_owner.ExitCode));
            }
        }

        public Task<ProcessExit> Wait() => _exit.Task;

        public void Stop(StopReason reason) => _stopped ??= reason;
    }
}

/// <summary>An isolation that records what happens to its leases.</summary>
internal sealed class RecordingIsolation : ISettingsIsolation
{
    public readonly List<string> Log = new();

    public IIsolationLease Prepare(MakemkvRunSettings settings)
    {
        Log.Add("prepare");
        return new Lease(this, settings);
    }

    sealed class Lease : IIsolationLease
    {
        readonly RecordingIsolation _owner;
        readonly MakemkvRunSettings _settings;
        public Lease(RecordingIsolation owner, MakemkvRunSettings settings) { _owner = owner; _settings = settings; }
        public IReadOnlyDictionary<string, string> Environment() => new Dictionary<string, string> { ["HOME"] = _settings.WorkDirectory };
        public string? ProfilePath() => _settings.ProfileXml is null ? null : _settings.WorkDirectory + "/profile.mmcp.xml";
        public void FirstOutput() => _owner.Log.Add("firstOutput");
        public void Release() => _owner.Log.Add("release");
    }
}

internal sealed class FixedLocator : IToolLocator
{
    readonly string? _makemkvcon;
    public FixedLocator(string? makemkvcon) { _makemkvcon = makemkvcon; }

    public ToolInfo Locate(ToolKind tool) => tool == ToolKind.Makemkvcon && _makemkvcon is { } p
        ? new ToolInfo(tool, p, null, new List<string>())
        : new ToolInfo(tool, null, null, new List<string>(), new BroMessage(MessageCode.ToolMissing, Severity.Warning, ("tool", JsonValue.Of(EnumWire.Name(tool)))));
}

internal sealed class CountingSink : IRunSink
{
    public int Events;
    public void Event(RobotEvent @event) => Events++;
}
