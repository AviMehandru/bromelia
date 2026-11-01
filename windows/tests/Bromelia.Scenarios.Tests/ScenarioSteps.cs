using Bromelia.Domain;
using Bromelia.Engine;
using Bromelia.Foundation;

namespace Bromelia.Scenarios.Tests;

/// <summary>
/// One scenario played against LocalEngine over TestCompositionRoot (decisions 238–243): the world, the scripted tools, the
/// jobs the steps named, and every event of one feed opened at start and drained all along (reopened after each restart,
/// the seqs going on; what a killed engine publishes after the kill never happened). Each action step makes the
/// LocalEngine call of decision 238; an action the engine refuses fails the scenario. A failure reads
/// <c>&lt;id&gt; step &lt;n&gt; (&lt;kind&gt;): &lt;what was expected&gt; / &lt;what was found&gt;</c>.
/// </summary>
public sealed class ScenarioRun : IDisposable
{
    /// <summary>How long any wait may take in real time.</summary>
    public static readonly TimeSpan Patience = TimeSpan.FromSeconds(10);

    public readonly string Id;
    public readonly JsonValue Scenario;
    public readonly ScenarioWorld World;
    public readonly ScenarioTools Tools;
    public readonly TestCompositionRoot Root;
    /// <summary>The jobs by the name a step gave them.</summary>
    public readonly Dictionary<string, Id> Jobs = new();
    readonly List<EngineEvent> _events = new();
    EventFeed? _feed;
    Task? _draining;
    /// <summary>The seq after which the current feed's events never happened (a kill).</summary>
    long? _cutoff;
    string? _killAt;
    string? _killed;
    string? _holdAt;
    readonly ManualResetEventSlim _held = new();
    readonly ManualResetEventSlim _release = new();

    public ScenarioRun(string id, JsonValue scenario)
    {
        Id = id;
        Scenario = scenario;
        World = new ScenarioWorld(scenario);
        Root = new TestCompositionRoot(makemkvcon: World.Installed("makemkvcon"), mkvmerge: World.Installed("mkvmerge"), launcher: new Deferred(this));
        Tools = new ScenarioTools(World, Root.Clock, Root.Fs);
        World.Prepare(Root);
        Root.Crash = Point;
        Tools.Crash = Point;
    }

    /// <summary>The root takes its launcher before the tools exist (they need its clock and files): this hands each start on.</summary>
    sealed class Deferred : Ports.IProcessLauncher
    {
        readonly ScenarioRun _run;
        public Deferred(ScenarioRun run) { _run = run; }
        public Ports.IRunningProcess Start(Ports.ProcessSpec spec) => _run.Tools.Start(spec);
    }

    /// <summary>Plays the scenario by id; throws <see cref="ScenarioFailure"/> with the full text when it fails.</summary>
    public static Task Play(string id) => Play(id, ScenarioCatalog.Load(id));

    public static async Task Play(string id, JsonValue scenario)
    {
        ScenarioRun run;
        try
        {
            run = new ScenarioRun(id, scenario);
        }
        catch (ScenarioFailure f)
        {
            throw new ScenarioFailure($"{id} given: {f.Message}");
        }
        using (run) await run.PlayAll();
    }

    public int StepNumber { get; private set; }
    public string StepKind { get; private set; } = "start";

    async Task PlayAll()
    {
        try
        {
            await Root.Start();
            await OpenFeed(null);
        }
        catch (Exception e) when (e is not ScenarioFailure)
        {
            throw Failed(new ScenarioFailure("the engine starts", Describe(e)));
        }
        var steps = Scenario["steps"]!.AsArray!;
        Arm(0);
        for (int i = 0; i < steps.Count; i++)
        {
            StepNumber = i + 1;
            var (kind, value) = steps[i].AsObject!.First();
            StepKind = kind;
            try
            {
                await Step(kind, value);
            }
            catch (ScenarioFailure f)
            {
                throw Failed(f);
            }
            catch (Exception e)
            {
                throw Failed(new ScenarioFailure($"{kind} to succeed", Describe(e)));
            }
        }
        if (_killAt != null) throw Failed(new ScenarioFailure($"the kill at {_killAt} to be followed by a restart", "the end of the steps"));
    }

    ScenarioFailure Failed(ScenarioFailure f) => new($"{Id} step {StepNumber} ({StepKind}): {f.Message}");

    static string Describe(Exception e) => e is Foundation.BroFailure f ? "error " + JsonText(Error(f.Error))
        : e.GetType().Name + ": " + e.Message + " " + e.StackTrace?.Split('\n').FirstOrDefault()?.Trim();

    public static string JsonText(JsonValue? v) => v == null ? "null" : System.Text.RegularExpressions.Regex.Replace(v.ToString(), @"\s*\n\s*", " ").Trim();

    public static JsonValue Error(BroError e) => e.Params.Members.Count > 0
        ? JsonValue.Of(("code", JsonValue.Of(e.Code)), ("params", e.Params))
        : JsonValue.Of(("code", JsonValue.Of(e.Code)));

    // ---- the steps ----

    async Task Step(string kind, JsonValue v)
    {
        var e = Root.Engine;
        switch (kind)
        {
            case "expect":
                await ScenarioExpect.Check(this, v);
                break;
            case "wait":
                await ScenarioExpect.Check(this, JsonValue.Of(("when", v)));
                break;
            case "rip":
                await Rip(v);
                break;
            case "insert":
                World.Insert(Root, v["drive"]!.AsString!, v["disc"] ?? World.Disc ?? throw new ScenarioFailure("a disc to insert", "none"));
                await Quiet(); // the engine has handled the OS's event (its drive scan) before the next step
                break;
            case "remove":
                World.Remove(Root, v["drive"]!.AsString!);
                await Quiet();
                break;
            case "openTray":
                World.OpenTray(Root, v["drive"]!.AsString!);
                await Quiet();
                break;
            case "eject":
                await e.Eject(await DriveId(v["drive"]!.AsString!));
                break;
            case "closeTray":
                await e.CloseTray(await DriveId(v["drive"]!.AsString!));
                break;
            case "keepApart":
                await e.KeepApart(Job(v.AsString!));
                break;
            case "cancel":
                await e.Cancel(Job(v.AsString!));
                break;
            case "decide":
            {
                var id = Job(v["job"]!.AsString!);
                // The question may not be asked yet (the job was just started): wait for it, as `wait` would.
                if ((await e.Job(id)).Job.Decision == null)
                    await ScenarioExpect.When(this, JsonValue.Of(("job", v["job"]!), ("state", JsonValue.Of("awaitingDecision"))));
                var decision = (await e.Job(id)).Job.Decision ?? throw new ScenarioFailure($"job {v["job"]!.AsString} awaiting a decision", "none");
                await e.Decide(id, decision.Id, v["answer"]!.AsString!);
                break;
            }
            case "move":
                await e.Move(Job(v["job"]!.AsString!), (int)v["by"]!.AsInteger!.Value);
                break;
            case "startNow":
                await e.StartNow(Job(v.AsString!));
                break;
            case "retry":
            {
                if (v["drive"] != null || v["titles"] != null || v["makemkvSettings"] != null)
                    throw new ScenarioFailure("retry with only job, fromStep and as (phase 3)", JsonText(v));
                var job = await e.Retry(Job(v["job"]!.AsString!), v["fromStep"]?.AsString is { } from ? EnumWire.Parse<Domain.StepKind>(from) : null);
                if (v["as"]?.AsString is { } name) Jobs[name] = job.Id;
                break;
            }
            case "advance":
                await Advance((long)Math.Round(v.AsNumber!.Value * 1000));
                break;
            case "kill":
                await Killed(); // armed since the start or the last restart (Arm)
                break;
            case "stop":
                await StopAt(v["at"]!.AsString!);
                break;
            case "restart":
                await Restart();
                break;
            case "unmount":
                await Root.Unmount(LibraryPath(v.AsString!));
                break;
            case "mount":
                await Root.Mount(LibraryPath(v.AsString!));
                break;
            case "attachLibrary":
                await e.AttachLibrary(Fill(v["path"]!.AsString!), v["name"]?.AsString ?? "");
                break;
            case "config":
            {
                var envelope = await e.Config();
                await e.UpdateConfig(ScenarioWorld.Fill(ScenarioWorld.MergePatch(envelope.Config.Document, v)), envelope.Version);
                break;
            }
            case "tools":
                foreach (var (tool, script) in v.AsObject!) World.SetScript(tool, script);
                break;
            case "write":
                Root.Fs.Put(Path(v["path"]!.AsString!), ScenarioWorld.FileBytes(v["text"] ?? v["bytes"]!));
                break;
            case "flip":
            {
                var path = Path(v["path"]!.AsString!);
                var bytes = Root.Fs.Read(path).ToArray();
                bytes[v["byte"]!.AsInteger!.Value] ^= 1;
                Root.Fs.Put(path, bytes);
                break;
            }
            case "delete":
                Root.Fs.Remove(Path(v.AsString!));
                break;
            case "swapDisc":
                World.Disc = ScenarioCatalog.Disc(v);
                break;
            case "open" or "choose":
                throw new ScenarioFailure($"{kind} (SessionService, phase 4)", "not built");
            case "cancelCountdown" or "mounted":
                throw new ScenarioFailure($"{kind} (AutomationService, phase 4)", "not built");
            case "api":
                throw new ScenarioFailure("api (the Server, phase 4)", "not built");
            case "verify" or "rescan" or "testStep" or "testNotification":
                throw new ScenarioFailure($"{kind} (phase 5)", "not built");
            case "repair":
                throw new ScenarioFailure("repair (phase 7)", "not built");
            default:
                throw new ScenarioFailure("a known step", kind);
        }
    }

    /// <summary>decision 238: an ISO, folder or file through startJob (chosen titles as a session snapshot); a drive through
    /// rip, or startJob when the rip is automatic or has titles.</summary>
    async Task Rip(JsonValue v)
    {
        var e = Root.Engine;
        if (v["session"] != null) throw new ScenarioFailure("rip.session (SessionService, phase 4)", "not built");
        if (v["tracks"] != null) throw new ScenarioFailure("rip.tracks (Transform, phase 5)", "not built");
        var options = new List<(string, JsonValue)>();
        if (v["mode"]?.AsString is { } mode) options.Add(("mode", JsonValue.Of(mode)));
        if (v["profile"]?.AsString is { } profile) options.Add(("profile", JsonValue.Of(profile)));
        string? library = null;
        if (v["outputFolder"]?.AsString is { } folder)
        {
            var path = Fill(folder);
            library = (await e.Libraries()).FirstOrDefault(l => l.Path.TrimEnd('/') == path.TrimEnd('/'))?.Id
                ?? throw new ScenarioFailure("rip to succeed", "error " + JsonText(Error(new BroError(MessageCode.Wire(MessageCode.LibraryNotAttached),
                    ("path", JsonValue.Of(path))))));
            options.Add(("library", JsonValue.Of(library)));
        }
        var titles = v["titles"]?.AsArray?.Select(t => (int)t.AsInteger!.Value).ToList();
        var automatic = v["automatic"]?.AsBool == true;
        JobDto job;
        if (v["drive"]?.AsString is { } driveName)
        {
            var driveId = await DriveId(driveName);
            if (!automatic && titles == null)
                job = await e.Rip(driveId, new RipOptionsDto(EnumWire.Parse<RipMode>(v["mode"]?.AsString), v["profile"]?.AsString, null, null,
                    v["outputFolder"]?.AsString is { } f ? Fill(f) : null));
            else
            {
                var drive = (await e.Drives()).First(d => d.Id == driveId);
                var disc = World.DriveNamed(driveName).Disc;
                job = await e.StartJob(new JobRequest(JobKind.VideoDisc, drive.Medium?.Label ?? disc?["volume"]?.AsString ?? drive.Name, automatic, driveId,
                    drive.Medium?.Generation, Options: JsonValue.Of(options.ToArray()),
                    Details: titles != null && disc != null ? DiscRequest.Encode(new DiscRequest(new DiscSource.Drive(driveId), Snapshot(disc, titles))) : null));
            }
        }
        else
        {
            DiscSource source = v["iso"] != null ? new DiscSource.Iso(ScenarioWorld.Iso)
                : v["folder"]?.AsString is { } f ? new DiscSource.Folder(Path(f))
                : v["file"]?.AsString is { } file ? new DiscSource.File(Path(file))
                : throw new ScenarioFailure("rip.iso, drive, folder or file", JsonText(v));
            var disc = World.Disc ?? throw new ScenarioFailure("given.disc for rip", "none");
            job = await e.StartJob(new JobRequest(JobKind.VideoDisc, disc["volume"]?.AsString ?? "", automatic, Options: JsonValue.Of(options.ToArray()),
                Details: DiscRequest.Encode(new DiscRequest(source, titles != null ? Snapshot(disc, titles) : null))));
        }
        if (v["as"]?.AsString is { } name) Jobs[name] = job.Id;
        else Jobs["job" + (Jobs.Count + 1)] = job.Id;
    }

    /// <summary>H1-1: chosen titles as SessionService will store them: the disc's listing and every title ticked or not.</summary>
    static SessionSnapshot Snapshot(JsonValue disc, IReadOnlyList<int> titles)
    {
        var listing = ScenarioWorld.ParsedListing(disc);
        return new SessionSnapshot(listing, new SessionChoices(listing.Titles.Select((_, i) => new TitleChoice(i, titles.Contains(i))).ToList()));
    }

    public Id Job(string name) => Jobs.TryGetValue(name, out var id) ? id : throw new ScenarioFailure("a job named " + name, "none");

    /// <summary>The engine's DriveId of a fake drive (by its device).</summary>
    public async Task<string> DriveId(string name)
    {
        var device = World.DriveNamed(name).Device;
        return (await Root.Engine.Drives()).FirstOrDefault(d => d.Device == device)?.Id
            ?? throw new ScenarioFailure($"drive {name} ({device}) in drives()", "not listed");
    }

    string LibraryPath(string id) =>
        (World.Configuration()["libraries"]?.AsArray ?? new List<JsonValue>()).FirstOrDefault(l => l["id"]?.AsString == id)?["path"]?.AsString
        ?? Path(id);

    /// <summary>A path the scenario gives: relative to the root, {root} filled in.</summary>
    public static string Path(string relative)
    {
        var p = Fill(relative);
        return p.StartsWith('/') ? p : TestCompositionRoot.Root + "/" + p;
    }

    static string Fill(string s) => s.Replace("{root}", TestCompositionRoot.Root);

    // ---- events ----

    async Task OpenFeed(long? after)
    {
        var feed = await Root.Engine.Events(after, Array.Empty<Id>());
        _feed = feed;
        _cutoff = null;
        _draining = Task.Run(async () =>
        {
            while (await feed.Next() is { } ev)
            {
                if (ev.Type == EngineEventType.Snapshot) continue;
                lock (_events)
                {
                    if (_feed != feed || _cutoff is { } cut && ev.Seq > cut) continue;
                    _events.Add(ev);
                }
            }
        });
    }

    /// <summary>The events so far (a copy).</summary>
    public IReadOnlyList<EngineEvent> Events()
    {
        lock (_events) return _events.ToList();
    }

    /// <summary>Until the feed has delivered what the bus published (it is read on its own task, and leaves out the jobLog
    /// events, so it can't be matched by seq): no new event three times in a row, 5 ms apart.</summary>
    public async Task CatchUp()
    {
        var deadline = DateTime.UtcNow + Patience;
        int seen = 0, count = -1;
        while (seen < 3 && DateTime.UtcNow < deadline)
        {
            await Task.Delay(5);
            int now;
            lock (_events) now = _events.Count;
            seen = now == count ? seen + 1 : 0;
            count = now;
        }
    }

    // ---- settling ----

    /// <summary>The executor's activity is only the parked workers, three times in a row: the parked processes' and, for
    /// each job awaiting a decision, its step waiting for the answer.</summary>
    public async Task Quiet()
    {
        var deadline = DateTime.UtcNow + Patience;
        int seen = 0;
        while (seen < 3)
        {
            if (DateTime.UtcNow > deadline)
                throw new ScenarioFailure("the engine to go quiet",
                    $"activity {Root.Executor.Activity}, parked processes {Tools.Parked()}, questions {AwaitingDecisions()}");
            await Task.Delay(5);
            seen = Root.Executor.Activity == Tools.Parked() + AwaitingDecisions() ? seen + 1 : 0;
        }
    }

    /// <summary>The jobs whose latest jobChanged or jobFinished says awaitingDecision.</summary>
    int AwaitingDecisions()
    {
        var states = new Dictionary<string, string?>();
        lock (_events)
            foreach (var ev in _events)
                if (ev.Type is EngineEventType.JobChanged or EngineEventType.JobFinished && ev.Data["job"]?["id"]?.AsString is { } id)
                    states[id] = ev.Data["job"]?["state"]?.AsString;
        return states.Values.Count(s => s == "awaitingDecision");
    }

    /// <summary>Decision 239: until the engine is quiet and every job has settled (finished, blocked, waiting for resources or
    /// awaiting a decision); while some job hasn't but the clock has a timer, the clock moves to it. A kill waiting for its
    /// point ends the wait once it has happened.</summary>
    public async Task Settle()
    {
        var deadline = DateTime.UtcNow + Patience;
        while (true)
        {
            if (_killed != null) return;
            await Quiet();
            if (_killed != null) return;
            var unsettled = (await AllJobs()).Where(j => j.State is not (JobState.Finished or JobState.Blocked or JobState.AwaitingDecision
                or JobState.WaitingForResources)).ToList();
            if (unsettled.Count == 0)
            {
                await Quiet();
                if ((await AllJobs()).All(j => j.State is JobState.Finished or JobState.Blocked or JobState.AwaitingDecision or JobState.WaitingForResources))
                {
                    await CatchUp();
                    return;
                }
                continue;
            }
            if (DateTime.UtcNow > deadline || Root.Clock.NextDue() is not { } due)
                throw new ScenarioFailure("every job to settle", "stuck: " + string.Join(", ", unsettled.Select(j =>
                    $"{NameOf(j.Id)} {EnumWire.Name(j.State)}{(j.Progress is { } p ? " at " + EnumWire.Name(p.Step) : "")}")));
            Root.Clock.Advance(Math.Max(0, due.UnixMilliseconds - Root.Clock.Now().UnixMilliseconds));
        }
    }

    public async Task<IReadOnlyList<JobDto>> AllJobs()
    {
        var all = new List<JobDto>();
        string? cursor = null;
        do
        {
            var page = await Root.Engine.Jobs(new JobQueryDto(Array.Empty<JobState>(), Array.Empty<Outcome>(), Cursor: cursor, Limit: JobQueryDto.MaxLimit));
            all.AddRange(page.Items);
            cursor = page.Next;
        }
        while (cursor != null);
        return all.OrderBy(j => j.CreatedAt.UnixMilliseconds).ThenBy(j => j.Id.Value, StringComparer.Ordinal).ToList();
    }

    public string NameOf(Id id) => Jobs.FirstOrDefault(j => j.Value == id).Key ?? id.Value;

    /// <summary>The clock moves to each due timer in turn (the engine going quiet after each), then to now + ms.</summary>
    async Task Advance(long ms)
    {
        var target = Root.Clock.Now().UnixMilliseconds + ms;
        await Quiet();
        while (Root.Clock.NextDue() is { } due && due.UnixMilliseconds <= target)
        {
            Root.Clock.Advance(Math.Max(0, due.UnixMilliseconds - Root.Clock.Now().UnixMilliseconds));
            await Quiet();
        }
        Root.Clock.Advance(Math.Max(0, target - Root.Clock.Now().UnixMilliseconds));
        await Quiet();
        await CatchUp();
    }

    // ---- crash points, kill, stop, restart ----

    /// <summary>Arms the kill or stop point of the steps from <paramref name="from"/> up to the next restart, so the engine
    /// stops there however soon it gets there (a job started by the step before a kill may reach its point before the kill
    /// step is played).</summary>
    void Arm(int from)
    {
        var steps = Scenario["steps"]!.AsArray!;
        for (int i = from; i < steps.Count; i++)
        {
            var (kind, value) = steps[i].AsObject!.First();
            if (kind == "restart") return;
            if (kind == "kill") _killAt = value["at"]!.AsString!;
            if (kind == "stop") _holdAt = value["at"]!.AsString!;
        }
    }

    /// <summary>inStep:&lt;kind&gt; is inStep:&lt;kind&gt;:1.</summary>
    static string Normal(string point) =>
        point.StartsWith("inStep:", StringComparison.Ordinal) && point.Count(c => c == ':') == 1 ? point + ":1" : point;

    /// <summary>Every crash point the engine (or a tool start) reaches: a kill there kills the engine; a stop there holds it
    /// until the stop has begun.</summary>
    void Point(string point)
    {
        if (point.StartsWith("beforeStep:", StringComparison.Ordinal)) Tools.StepBegins(point[11..]);
        else if (point.StartsWith("afterStep:", StringComparison.Ordinal)) Tools.StepBegins(null);
        if (_killAt != null && _killed == null && Normal(point) == Normal(_killAt))
        {
            _killed = point;
            Tools.EndAll();
            var thrown = Root.Kill(point);
            lock (_events) _cutoff = Root.KilledSeq;
            throw thrown;
        }
        if (_holdAt != null && Normal(point) == Normal(_holdAt))
        {
            _holdAt = null;
            _held.Set();
            _release.Wait(TimeSpan.FromSeconds(60));
        }
    }

    /// <summary>H1-6: the engine is held at the point, LocalEngine.stop begins, the point is released, the stop ends. A
    /// point never reached, or a stop that hits the 30 s cap, fails the scenario.</summary>
    async Task StopAt(string point)
    {
        var deadline = DateTime.UtcNow + Patience;
        while (!_held.IsSet)
        {
            if (DateTime.UtcNow > deadline) break;
            await Task.Delay(5);
            if (!_held.IsSet && Root.Executor.Activity == Tools.Parked()
                && (await AllJobs()).All(j => j.State is JobState.Finished or JobState.Blocked or JobState.AwaitingDecision) && !_held.IsSet)
                break;
        }
        if (!_held.IsSet)
        {
            _holdAt = null;
            throw new ScenarioFailure($"the engine to reach {point}", "it never did");
        }
        var stopping = Task.Run(Root.Stop);
        await Task.WhenAny(Root.Executor.Barrier(), Task.Delay(200));
        _release.Set();
        await stopping;
        _feed = null;
        if (Root.LastStop >= TestCompositionRoot.StopCap)
            throw new ScenarioFailure("the stop to end within 30 s", $"{Root.LastStop.TotalSeconds:0.0} s (the cap)");
    }

    /// <summary>Until the engine has died at the armed kill point (the engine runs, settling, until then); a point it never
    /// reaches fails the scenario.</summary>
    async Task Killed()
    {
        if (_killed != null) return;
        await Settle();
        if (_killed == null)
            throw new ScenarioFailure($"the engine to reach {_killAt}", "it never did; jobs " + string.Join(", ", (await AllJobs()).Select(j =>
                $"{NameOf(j.Id)} {EnumWire.Name(j.State)} {(j.Outcome is { } o ? EnumWire.Name(o) : "")} {JsonText(j.Error is { } er ? Error(er) : null)}"))
                + "; makemkvcon " + string.Join(" | ", Tools.Calls("makemkvcon")));
    }

    /// <summary>After a kill: once the point was reached, the killed engine is finished off and a new one starts on the same
    /// database and files. Otherwise the engine settles, stops (a stop that hits the 30 s cap fails) and a new one starts.
    /// The feed reopens after the last seq the old bus had.</summary>
    async Task Restart()
    {
        if (_killAt != null) await Killed();
        else if (_feed != null) await Settle();
        await CatchUp();
        long after = Root.Bus.LastSeq;
        var old = _feed;
        _feed = null;
        old?.Close();
        await Root.Restart();
        _killAt = null;
        _killed = null;
        _held.Reset();
        _release.Reset();
        Tools.StepBegins(null);
        await OpenFeed(after);
        Arm(StepNumber);
    }

    public void Dispose()
    {
        _release.Set();
        _feed?.Close();
        Root.Dispose();
    }
}
