using System.Text;
using System.Threading.Channels;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Bromelia.TestKit;
using Bromelia.Tests;

namespace Bromelia.Scenarios.Tests;

/// <summary>
/// The scripted tools (decision 236): a ProcessLauncher that plays the world's tool scripts (shared/scenarios/README.md
/// "Tool scripts") instead of starting anything. The tool is the executable's name. makemkvcon's drive scan
/// (<c>info disc:9999</c>) is answered from the drive world; a makemkvcon backup whose destination exists is refused as the
/// real one refuses it; otherwise the first rule whose <c>when</c> matches writes its files, then prints its lines at once.
/// It keeps the ProcessLauncher contract the way the platform launchers do, on the ManualClock: a rule's silence, the stall
/// watchdog (spec.stallTimeout after the last line), the stop escalation (TERM first, or INT first: then TERM, then KILL,
/// 5 s apart; a signal the script ignores does nothing; makemkvcon ignores SIGINT), a process that outlives SIGKILL
/// abandoned 30 s later, and the transcript (the platform launchers' header, every line and the ending). Every launch but
/// the drive scans is recorded (<see cref="Calls"/>), and each one inside a step reports the crash point
/// inStep:&lt;kind&gt;:&lt;n&gt; (the step's n-th process) to <see cref="Crash"/> once its rule's files are written, before
/// any line. Like the real makemkvcon, the line
/// <c>saved</c> of a run that wrote titles reports how many it wrote (lines.yaml's says 1; owner question H3a-1).
/// </summary>
public sealed class ScenarioTools : IProcessLauncher
{
    static readonly Duration Grace = new(5);
    static readonly Duration Abandon = new(30);

    readonly ScenarioWorld _world;
    readonly ManualClock _clock;
    readonly MemoryFileSystem _fs;
    readonly IReadOnlyDictionary<string, string> _named = ScenarioCatalog.Lines();
    readonly object _lock = new();
    readonly List<string> _calls = new();
    readonly List<Process> _alive = new();
    /// <summary>The backups the tools made (the destination's folder → the disc backed up): reading anything there reads that
    /// disc, whatever the engine renamed the backup to (an image for a folder, a folder for an image).</summary>
    readonly Dictionary<string, JsonValue> _backups = new();
    string? _step;
    int _inStep;

    /// <summary>Hears inStep:&lt;kind&gt;:&lt;n&gt; before a process starts; what it throws, Start throws (a kill).</summary>
    public Action<string>? Crash;

    public ScenarioTools(ScenarioWorld world, ManualClock clock, MemoryFileSystem fs)
    {
        _world = world;
        _clock = clock;
        _fs = fs;
    }

    /// <summary>A step of this kind begins (its beforeStep point): the processes started from now on are its.</summary>
    public void StepBegins(string? kind)
    {
        lock (_lock)
        {
            _step = kind;
            _inStep = 0;
        }
    }

    /// <summary>Every launch but the drive scans, as "&lt;tool&gt; &lt;arguments joined by spaces&gt;", of one tool.</summary>
    public IReadOnlyList<string> Calls(string tool)
    {
        lock (_lock) return _calls.Where(c => c.StartsWith(tool + " ", StringComparison.Ordinal) || c == tool).ToList();
    }

    /// <summary>The processes that haven't ended and whose lines have all been read: each one parks the step's worker that
    /// waits for it (it ends only when the clock moves or it is stopped).</summary>
    public int Parked()
    {
        lock (_lock) return _alive.Count(p => p.Drained);
    }

    /// <summary>A kill: every process ends at once, without another line or transcript line.</summary>
    public void EndAll()
    {
        List<Process> alive;
        lock (_lock)
        {
            alive = _alive.ToList();
            _alive.Clear();
        }
        foreach (var p in alive) p.Killed();
    }

    public IRunningProcess Start(ProcessSpec spec)
    {
        var tool = spec.Executable[(spec.Executable.LastIndexOfAny(new[] { '/', '\\' }) + 1)..];
        if (tool.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)) tool = tool[..^4];
        if (tool == "makemkvcon" && spec.Arguments.Contains("disc:9999"))
            return Begin(new Process(this, spec, tool, new Run(_world.DriveScan().ToList(), new List<string>(), 0, null, null, Array.Empty<string>())));
        string? point = null;
        lock (_lock)
        {
            _calls.Add(string.Join(" ", new[] { tool }.Concat(spec.Arguments)));
            if (_step != null) point = $"inStep:{_step}:{++_inStep}";
        }
        var script = _world.Script(tool)
            ?? throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.ProcessCouldNotStart), ("program", JsonValue.Of(spec.Executable)),
                ("reason", JsonValue.Of("no script"))));
        // The process has started (its rule's files are written at its start) when the engine dies at the point.
        var run = Plan(tool, spec, script);
        if (point != null) Crash?.Invoke(point);
        return Begin(new Process(this, spec, tool, run));
    }

    Process Begin(Process process)
    {
        lock (_lock) _alive.Add(process);
        process.Begin();
        return process;
    }

    void Ended(Process process)
    {
        lock (_lock) _alive.Remove(process);
    }

    // ---- what a launch does ----

    /// <summary>What a rule makes a launch do: its lines (stdout, stderr), its exit (null: never), its silence before that,
    /// the signals it ignores.</summary>
    sealed record Run(List<string> Stdout, List<string> Stderr, int? Exit, double? Silence, IReadOnlyList<string>? Ignore,
        IReadOnlyList<string> Writes);

    /// <summary>The arguments of a makemkvcon run: its command (info, mkv, backup, …), source, title and destination.</summary>
    sealed record Args(string Command, string Source, string Title, string Dest, int? MinLength);

    static Args MakemkvArgs(IReadOnlyList<string> a)
    {
        int at = -1;
        for (int i = 0; i < a.Count; i++)
            if (a[i] is "info" or "mkv" or "backup" or "reg" or "f")
            {
                at = i;
                break;
            }
        int? minLength = a.FirstOrDefault(x => x.StartsWith("--minlength=", StringComparison.Ordinal)) is { } m ? int.Parse(m[12..]) : null;
        if (at < 0) return new Args(a.FirstOrDefault() ?? "", "", "", "", minLength);
        var rest = a.Skip(at + 1).Where(x => x != "--decrypt").ToList();
        var command = a[at];
        return command switch
        {
            "mkv" => new Args(command, rest.ElementAtOrDefault(0) ?? "", rest.ElementAtOrDefault(1) ?? "", rest.ElementAtOrDefault(2) ?? "", minLength),
            "backup" => new Args(command, rest.ElementAtOrDefault(0) ?? "", "", rest.ElementAtOrDefault(1) ?? "", minLength),
            _ => new Args(command, rest.ElementAtOrDefault(0) ?? "", "", "", minLength),
        };
    }

    Run Plan(string tool, ProcessSpec spec, JsonValue script)
    {
        var a = spec.Arguments;
        var mk = tool == "makemkvcon" ? MakemkvArgs(a) : null;
        var command = mk?.Command ?? a.FirstOrDefault() ?? "";
        var input = Option(a, "-i") ?? (command == "-J" ? a.ElementAtOrDefault(1) : null);
        var output = Option(a, "-o");
        var dest = mk?.Dest ?? output ?? spec.WorkingDirectory ?? TestCompositionRoot.DataDir;
        var debugLog = (spec.WorkingDirectory ?? TestCompositionRoot.DataDir) + "/MakeMKV_log.txt";
        string Fill(string text, string title = "")
        {
            text = text.Replace("{title}", title.Length > 0 ? title : mk?.Title ?? "").Replace("{dest}", dest).Replace("{debugLog}", debugLog)
                .Replace("{device}", mk != null ? _world.DeviceOf(mk.Source) : "").Replace("{input}", input ?? "").Replace("{output}", output ?? "");
            for (int i = 0; i < a.Count; i++) text = text.Replace("{arg" + i + "}", a[i]);
            foreach (var (name, value) in spec.Environment) text = text.Replace("{env:" + name + "}", value);
            return text;
        }

        var stdout = new List<string>();
        var stderr = new List<string>();
        var writes = new List<string>();
        if (mk is { Command: "backup" } && _fs.Exists(mk.Dest))
        {
            foreach (var name in new[] { "backupFolderExists", "backupFailed", "backupFailedSummary" }) stdout.Add(Fill(_named[name]));
            return new Run(stdout, stderr, 0, null, null, writes);
        }
        var rule = (script["rules"]?.AsArray ?? new List<JsonValue>()).FirstOrDefault(r => Matches(r["when"], command, mk, a, input));
        if (rule == null)
        {
            stderr.Add($"{tool}: no rule of its script matches {string.Join(" ", a)}");
            return new Run(stdout, stderr, 127, null, null, writes);
        }
        var disc = mk != null ? DiscOf(mk.Source) : _world.Disc;
        if (mk is { Command: "backup" } && disc != null)
            lock (_lock) _backups[mk.Dest[..Math.Max(0, mk.Dest.LastIndexOf('/'))]] = disc;

        if (rule["writeDebugLog"]?.AsString is { } debugText) Write(debugLog, Encoding.UTF8.GetBytes(debugText), writes);
        List<int>? written = null;
        if (rule["writeTitles"] is { } wt && mk != null && disc != null)
        {
            var count = (ScenarioWorld.Listing(disc, mk.MinLength).Split('\n').Count(l => l.StartsWith("TINFO:", StringComparison.Ordinal) && l.Contains(",27,0,")));
            var only = wt["titles"]?.AsArray?.Select(t => (int)t.AsInteger!.Value).ToHashSet();
            var titles = mk.Title == "all" ? Enumerable.Range(0, count).Where(i => only == null || only.Contains(i)).ToList()
                : int.TryParse(mk.Title, out var t) ? new List<int> { t } : new List<int>();
            written = titles;
            foreach (var i in titles)
                Write(dest + "/" + $"title_t{i:00}.mkv", Encoding.UTF8.GetBytes(Fill(wt["content"]?.AsString ?? "", i.ToString())), writes);
        }
        foreach (var w in rule["write"]?.AsArray ?? new List<JsonValue>())
        {
            var path = Fill(w["path"]!.AsString!);
            if (!path.StartsWith('/')) path = dest + "/" + path;
            byte[] bytes = w["copyOf"]?.AsString == "input" && input != null && _fs.Exists(input) ? _fs.Read(input)
                : w["bytes"] is { } b ? ScenarioWorld.FileBytes(b) : Encoding.UTF8.GetBytes(Fill(w["content"]?.AsString ?? ""));
            Write(path, bytes, writes);
        }
        if (rule["writeIso"] is { } iso) Write(dest, ScenarioWorld.FileBytes(iso), writes);
        foreach (var (path, content) in rule["writeFolder"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
            Write(dest + "/" + path, Encoding.UTF8.GetBytes(Fill(content.AsString ?? "")), writes);

        foreach (var line in rule["print"]?.AsArray ?? new List<JsonValue>())
        {
            var text = line.AsString!;
            if (text == "listing")
            {
                if (disc != null) stdout.AddRange(ScenarioWorld.Listing(disc, mk?.MinLength).Split('\n', StringSplitOptions.RemoveEmptyEntries));
            }
            else if (text.StartsWith("fixture:", StringComparison.Ordinal))
                stdout.AddRange(Fixtures.Text(text[8..]).Split('\n').Select(l => l.TrimEnd('\r')).Where(l => l.Length > 0));
            else if (text == "saved" && written != null && written.Count != 1)
                stdout.Add(_named[text].Replace("Copy complete. 1 titles", $"Copy complete. {written.Count} titles").Replace(",\"1\"", $",\"{written.Count}\""));
            else stdout.Add(Fill(_named.TryGetValue(text, out var named) ? named : text));
        }
        foreach (var line in rule["stderr"]?.AsArray ?? new List<JsonValue>()) stderr.Add(Fill(line.AsString!));
        if (rule["probe"] is { } probe && input != null)
            foreach (var (glob, answer) in probe.AsObject!)
                if (Glob(glob, Name(input)))
                {
                    stdout.Add(ProbeJson(answer["durationSeconds"]?.AsNumber ?? 0, answer["tracks"]?.AsArray?.Select(t => t.AsString!).ToList() ?? new List<string>(),
                        (int)(answer["chapters"]?.AsInteger ?? 0)));
                    break;
                }
        if (rule["matchListing"]?.AsBool == true && input != null && _world.Disc is { } listed) stdout.Add(ListedProbe(listed, Name(input)));

        int? exit = rule["exit"] switch
        {
            null => 0,
            JsonValue.String { Value: "never" } => null,
            { } e => (int)e.AsInteger!.Value,
        };
        var ignore = rule["ignore"]?.AsArray?.Select(s => s.AsString!).ToList() ?? (tool == "makemkvcon" ? new List<string> { "SIGINT" } : null);
        return new Run(stdout, stderr, exit, rule["silence"]?.AsNumber, ignore, writes);
    }

    /// <summary>The disc a makemkvcon source reads: a backup the tools made holds the disc it was made from; otherwise the
    /// world's.</summary>
    JsonValue? DiscOf(string source)
    {
        var path = source.StartsWith("file:", StringComparison.Ordinal) || source.StartsWith("iso:", StringComparison.Ordinal) ? source[(source.IndexOf(':') + 1)..] : null;
        if (path != null)
            lock (_lock)
                foreach (var (dest, disc) in _backups)
                    if (path.StartsWith(dest + "/", StringComparison.Ordinal)) return disc;
        return _world.DiscOf(source);
    }

    static string? Option(IReadOnlyList<string> a, string name)
    {
        int i = a.ToList().IndexOf(name);
        return i >= 0 && i + 1 < a.Count ? a[i + 1] : null;
    }

    static string Name(string path) => path[(path.LastIndexOf('/') + 1)..];

    static bool Matches(JsonValue? when, string command, Args? mk, IReadOnlyList<string> a, string? input)
    {
        if (when is null) return true;
        if (when["command"]?.AsString is { } c && c != command) return false;
        if (when["title"]?.AsString is { } t && mk?.Title != t) return false;
        if (when["argsInclude"]?.AsArray is { } include && include.Any(x => !a.Contains(x.AsString!))) return false;
        if (when["file"]?.AsString is { } file && (input is null || !Glob(file, input))) return false;
        return true;
    }

    /// <summary>A glob over a whole string: * any run of characters, ? one.</summary>
    static bool Glob(string pattern, string text) =>
        System.Text.RegularExpressions.Regex.IsMatch(text,
            "^" + System.Text.RegularExpressions.Regex.Escape(pattern).Replace("\\*", ".*").Replace("\\?", ".") + "$");

    /// <summary>mkvmerge -J's answer, in the shape MkvProbe reads.</summary>
    static string ProbeJson(double seconds, IReadOnlyList<string> tracks, int chapters) =>
        JsonValue.Of(
            ("container", JsonValue.Of(("properties", JsonValue.Of(("duration", JsonValue.Of((long)Math.Round(seconds * 1e9))))))),
            ("tracks", JsonValue.Of(tracks.Select((type, i) => JsonValue.Of(("id", JsonValue.Of(i)), ("type", JsonValue.Of(type)))))),
            ("chapters", JsonValue.Of(chapters > 0 ? new[] { JsonValue.Of(("num_entries", JsonValue.Of(chapters))) } : Array.Empty<JsonValue>())))
        .ToString().TrimEnd('\n');

    /// <summary>What the listing says of the title a file came from (title_t&lt;i&gt;.mkv, else the first): its duration,
    /// a video and an audio track, its chapters.</summary>
    static string ListedProbe(JsonValue disc, string file)
    {
        var titles = disc["titles"]?.AsArray ?? new List<JsonValue>();
        var m = System.Text.RegularExpressions.Regex.Match(file, @"title_t(\d+)");
        var title = titles.ElementAtOrDefault(m.Success ? int.Parse(m.Groups[1].Value) : 0) ?? JsonValue.Of();
        return ProbeJson(ScenarioWorld.Seconds(title["duration"]?.AsString ?? "0"), new[] { "video", "audio" }, (int)(title["chapters"]?.AsInteger ?? 2));
    }

    void Write(string path, byte[] bytes, List<string> writes)
    {
        if (_fs.Frozen) return;
        _fs.Put(path, bytes);
        writes.Add(path);
    }

    /// <summary>Appends a line to a transcript (not once the files are frozen: a killed engine's).</summary>
    void Append(string? transcript, string line)
    {
        if (transcript is not { Length: > 0 } path || _fs.Frozen) return;
        var before = _fs.Exists(path) ? _fs.Read(path) : Array.Empty<byte>();
        _fs.Put(path, before.Concat(Encoding.UTF8.GetBytes(line + "\n")).ToArray());
    }

    /// <summary><c>exit status 0</c>, <c>signal 15</c> or <c>abandoned</c>, then <c>, stalled</c> and <c>, cancelled</c>, as the
    /// platform launchers end a transcript.</summary>
    static string Ending(ProcessExit exit)
    {
        var s = exit.Abandoned ? "abandoned" : exit.Signal is { } sig ? $"signal {sig}" : $"exit status {exit.Status}";
        if (exit.Stalled is not null) s += ", stalled";
        if (exit.Cancelled) s += ", cancelled";
        return s;
    }

    static int SignalNumber(string signal) => signal switch
    {
        "SIGINT" => 2,
        "SIGKILL" => 9,
        _ => 15,
    };

    // ---- a process ----

    sealed class Process : IRunningProcess
    {
        readonly ScenarioTools _tools;
        readonly ProcessSpec _spec;
        readonly Run _run;
        readonly Channel<OutputLine> _lines = Channel.CreateUnbounded<OutputLine>();
        readonly TaskCompletionSource<ProcessExit> _exit = new(TaskCreationOptions.RunContinuationsAsynchronously);
        readonly object _gate = new();
        readonly List<ITimerHandle> _timers = new();
        StopReason? _reason;
        Duration? _stalled;
        bool _ended;
        int _read;
        int _written;

        public Process(ScenarioTools tools, ProcessSpec spec, string tool, Run run)
        {
            _tools = tools;
            _spec = spec;
            _run = run;
        }

        /// <summary>Its lines are all read (it may still be running).</summary>
        public bool Drained => Volatile.Read(ref _read) == Volatile.Read(ref _written);

        public void Begin()
        {
            _tools.Append(_spec.Transcript,
                $"==== {Instant.Format(_tools._clock.Now())} $ {string.Join(" ", new[] { _spec.Executable }.Concat(_spec.Arguments).Select(ArgumentSplitter.Quote))}");
            var at = _tools._clock.Now();
            foreach (var line in _run.Stdout) Emit(new OutputLine(OutputSource.Stdout, line, at));
            foreach (var line in _run.Stderr) Emit(new OutputLine(OutputSource.Stderr, line, at));
            if (_run.Silence is { } silence && silence > 0)
            {
                lock (_gate) _timers.Add(After(new Duration(silence), Natural));
                Watch();
            }
            else if (_run.Exit is null) Watch();
            else Natural();
        }

        void Emit(OutputLine line)
        {
            Interlocked.Increment(ref _written);
            _lines.Writer.TryWrite(line);
            _tools.Append(_spec.Transcript, line.Text);
        }

        ITimerHandle After(Duration d, Action then) =>
            _tools._clock.Timer(new TimerSchedule.At(new Instant(_tools._clock.Now().UnixMilliseconds + (long)Math.Round(d.Seconds * 1000))), then);

        /// <summary>The stall watchdog: it is quiet from now on, so it is stopped as stalled once the timeout has passed.</summary>
        void Watch()
        {
            if (_spec.StallTimeout is not { } limit) return;
            lock (_gate)
                _timers.Add(After(limit, () =>
                {
                    lock (_gate)
                    {
                        if (_ended || _reason != null) return;
                        _stalled = limit;
                    }
                    Stop(StopReason.Stalled);
                }));
        }

        /// <summary>The end its script gives it, unless it was stopped meanwhile (exit: never stays running).</summary>
        void Natural()
        {
            lock (_gate)
                if (_reason != null) return;
            if (_run.Exit is { } status) End(new ProcessExit(status));
        }

        public async IAsyncEnumerable<OutputLine> Lines()
        {
            // A real hop to the thread pool first (never xUnit's synchronization context).
            await Task.Run(() => { }).ConfigureAwait(false);
            while (await _lines.Reader.WaitToReadAsync().ConfigureAwait(false))
                while (_lines.Reader.TryRead(out var line))
                {
                    Interlocked.Increment(ref _read);
                    yield return line;
                }
        }

        public Task<ProcessExit> Wait() => _exit.Task;

        public void Stop(StopReason reason)
        {
            lock (_gate)
            {
                if (_ended || _reason != null) return;
                _reason = reason;
            }
            var signals = _spec.StopPolicy == StopPolicy.InterruptFirst ? new[] { "SIGINT", "SIGTERM", "SIGKILL" } : new[] { "SIGTERM", "SIGKILL" };
            Signal(signals, 0);
        }

        /// <summary>Sends signals[i]: one it doesn't ignore ends it; otherwise the next comes 5 s later, and 30 s after an
        /// ignored SIGKILL it is abandoned.</summary>
        void Signal(string[] signals, int i)
        {
            lock (_gate)
                if (_ended) return;
            var signal = signals[i];
            if (_run.Ignore?.Contains(signal) != true)
            {
                End(new ProcessExit(-1, SignalNumber(signal), _stalled, false, Cancelled()));
                return;
            }
            lock (_gate)
                _timers.Add(i + 1 < signals.Length
                    ? After(Grace, () => Signal(signals, i + 1))
                    : After(Abandon, () => End(new ProcessExit(-1, null, _stalled, true, Cancelled()))));
        }

        bool Cancelled()
        {
            lock (_gate) return _reason is StopReason.Cancelled or StopReason.Shutdown;
        }

        void End(ProcessExit exit)
        {
            lock (_gate)
            {
                if (_ended) return;
                _ended = true;
                foreach (var t in _timers) t.Cancel();
            }
            _tools.Append(_spec.Transcript, $"==== {Instant.Format(_tools._clock.Now())} {Ending(exit)}");
            _tools.Ended(this);
            _lines.Writer.TryComplete();
            _exit.TrySetResult(exit);
        }

        /// <summary>The engine was killed: it ends at once, without another line.</summary>
        public void Killed()
        {
            lock (_gate)
            {
                if (_ended) return;
                _ended = true;
                foreach (var t in _timers) t.Cancel();
            }
            _lines.Writer.TryComplete();
            _exit.TrySetResult(new ProcessExit(-1, 9));
        }
    }
}
