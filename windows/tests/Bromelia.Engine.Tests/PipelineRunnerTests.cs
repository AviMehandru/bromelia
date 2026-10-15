using System.Text;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Engine.Tests;

/// <summary>shared/fixtures/engine/pipeline-runner.cases.json and job-planner.cases.json.</summary>
public sealed class PipelineRunnerTests
{
    static readonly TimeSpan Patience = TimeSpan.FromSeconds(10);

    static JsonValue EmptyObject => JsonValue.Of(Array.Empty<(string, JsonValue)>());

    static MakemkvNotice? Notice(string? name) => name switch
    {
        "keyExpired" => new MakemkvNotice.KeyExpired(),
        "evaluationNotStarted" => new MakemkvNotice.EvaluationNotStarted(),
        "versionTooOld" => new MakemkvNotice.VersionTooOld(),
        "libreDriveRequired" => new MakemkvNotice.LibreDriveRequired(),
        _ => null,
    };

    static BroMessage Message(JsonValue j, Severity severity = Severity.Info) =>
        new(MessageCode.Parse(j["code"]!.AsString!)!.Value, (JsonValue.Object)(j["params"] ?? EmptyObject), severity);

    static BroError Error(JsonValue j) => new(j["code"]!.AsString!, (JsonValue.Object)(j["params"] ?? EmptyObject));

    static List<string> Strings(JsonValue? j) => j?.AsArray?.Select(s => s.AsString!).ToList() ?? new List<string>();

    /// <summary>A step that does what the case says.</summary>
    sealed class FakeStep : IStep
    {
        readonly Harness _h;
        readonly JsonValue _b;

        public FakeStep(Harness h, StepKind kind, JsonValue behaviour)
        {
            _h = h;
            Kind = kind;
            _b = behaviour;
        }

        public StepKind Kind { get; }
        public Resumability Resumability => Resumability.Idempotent;

        public ResourceSet Resources(JobSnapshot job)
        {
            var r = _b["resources"];
            var drives = Strings(r?["drives"]);
            if (_b["drive"]?.AsBool == true && job.Request.DriveId is { } d) drives.Add(d);
            return new ResourceSet(drives, r?["acquisition"]?.AsBool == true, Strings(r?["libraryWrites"]), (int)(r?["cpu"]?.AsInteger ?? 0),
                Strings(r?["io"]));
        }

        public async Task<StepReport> Run(StepContext context)
        {
            foreach (var p in _b["progress"]?.AsArray ?? new List<JsonValue>())
                context.Progress.Report(p.AsArray![0].AsNumber!.Value, new BroMessage(MessageCode.Parse(p.AsArray[1].AsString!)!.Value));
            foreach (var code in Strings(_b["log"])) context.Log.Log(new BroMessage(MessageCode.Parse(code)!.Value));
            var data = _b["output"] ?? EmptyObject;
            if (_b["config"]?.AsString is { } key)
                data = JsonValue.Of((key, context.Config.Document[key] ?? JsonValue.Null.Instance));
            if (_b["echo"]?.AsString is { } echo)
                data = context.Job.Outputs.Last(o => EnumWire.Name(o.Kind) == echo).Data;
            if (_b["ask"] is { } ask)
            {
                var question = new DecisionDto(ask["id"]!.AsString!, Message(ask["question"]!),
                    Strings(ask["answers"]).Select(a => new DecisionAnswer(a, new BroMessage(MessageCode.Parse("decision.answer." + a)!.Value))).ToList());
                var answer = await context.Decisions.Ask(question);
                data = JsonValue.Of(("answer", JsonValue.Of(answer)));
            }
            if (_b["gate"]?.AsString is { } gate) await _h.Gate(gate, context.Job.Id, context.Cancel);
            return new StepReport(
                data,
                AppendSteps: Strings(_b["append"]).Select(k => EnumWire.Parse<StepKind>(k)!.Value).ToList(),
                Skipped: _b["skipped"]?.AsBool == true,
                ReadErrors: (int)(_b["readErrors"]?.AsInteger ?? 0),
                Quarantined: _b["quarantined"]?.AsBool == true,
                Notice: Notice(_b["notice"]?.AsString),
                SkipJob: _b["skipJob"] is { } skip ? Message(skip, Severity.Warning) : null,
                Error: _b["fail"] is { } fail ? Error(fail) : null,
                AffectsOutcome: _b["affectsOutcome"]?.AsBool ?? true);
        }
    }

    /// <summary>Gives the document the case sets (given.config, then the config ops); read on the executor.</summary>
    sealed class CaseConfig : IConfigSource
    {
        public JsonValue Document = JsonValue.Of(Array.Empty<(string, JsonValue)>());

        public Config Current() => new(Document, new List<Issue>());
    }

    sealed class Harness : IDisposable
    {
        public readonly EngineClockTests.World W = new();
        public readonly EventBus Bus;
        public readonly Scheduler Scheduler;
        public readonly MemoryStore Store = new();
        public readonly MemoryFileSystem Fs = new();
        public readonly CaseConfig Config = new();
        public readonly PipelineRunner Runner;
        public readonly Dictionary<string, Id> Jobs = new();
        public readonly Dictionary<string, Decided> Outcomes = new();
        public readonly List<EngineEvent> Events = new();
        readonly object _gateLock = new();
        readonly Dictionary<string, TaskCompletionSource> _gates = new();
        readonly Dictionary<Id, int> _atGate = new();

        public Harness(JsonValue given)
        {
            Bus = new EventBus(W.Clock);
            Scheduler = new Scheduler(W.Executor, Bus, new SchedulerLimits());
            if (given["config"] is { } config) Config.Document = config;
            var steps = new List<IStep>();
            foreach (var kind in Enum.GetValues<StepKind>())
                steps.Add(new FakeStep(this, kind, given["steps"]?[EnumWire.Name(kind)] ?? EmptyObject));
            var env = new StepEnvironment(new ToolSet(null!, null!), Fs, W.Clock, null!, null!, null!, Config, null!, null!, "/data");
            Runner = new PipelineRunner(W.Executor, Bus, Scheduler, Store, new StepRegistry(steps), env, W.EngineClock);
            W.OnExecutor(() => Bus.Subscribe(null, Events.Add));
            int n = 0;
            foreach (var j in given["jobs"]!.AsArray!)
            {
                var id = new Id($"00000000-0000-4000-8000-{++n:x12}");
                var name = j["name"]!.AsString!;
                Jobs[name] = id;
                var kind = EnumWire.Parse<JobKind>(j["kind"]?.AsString ?? "videoDisc")!.Value;
                var title = j["title"]?.AsString ?? name;
                var request = new JobRequest(kind, title, j["automatic"]?.AsBool == true, j["driveId"]?.AsString);
                var plan = j["plan"] is { } p ? new JobPlan(Strings(p).Select(k => EnumWire.Parse<StepKind>(k)!.Value).ToList()) : null;
                var queue = EnumWire.Parse<Queue>(j["queue"]?.AsString ?? "acquisition")!.Value;
                var record = new JobRecord(id, kind, null, JobState.Queued, null, null, null, null, queue, 0, request.DriveId, null,
                    request.Automatic, null, title, null, request, plan, null, W.Clock.Now());
                ((IJobRepository)Store).Insert(record).Wait();
                int seq = 0;
                foreach (var d in j["done"]?.AsArray ?? new List<JsonValue>())
                {
                    var k = EnumWire.Parse<StepKind>(d["kind"]!.AsString)!.Value;
                    var state = EnumWire.Parse<StepState>(d["state"]?.AsString ?? "succeeded")!.Value;
                    ((IStepRepository)Store).Save(new StepRecord(id, seq++, k, state, 1, W.Clock.Now(), W.Clock.Now(),
                        new StepOutput(k, d["output"] ?? EmptyObject))).Wait();
                }
                if (j["log"] is { } log)
                {
                    long line = 0;
                    var lines = Strings(log).Select(c => new LogLineDto(++line, W.Clock.Now(), Severity.Info, MessageCode.Parse(c)!.Value,
                        new JsonValue.Object(new List<KeyValuePair<string, JsonValue>>()))).ToList();
                    Fs.CreateDirectory($"/data/jobs/{id.Value}", parentsMustExist: false);
                    Fs.WriteAtomically($"/data/jobs/{id.Value}/log.jsonl", JobLog.Encode(lines), 0x180);
                }
            }
        }

        readonly Dictionary<string, List<Id>> _waiting = new(); // gate → jobs waiting there
        readonly HashSet<string> _opened = new();
        readonly HashSet<Id> _cancelled = new();
        readonly HashSet<string> _started = new();

        public async Task Gate(string name, Id job, Foundation.CancellationToken cancel)
        {
            TaskCompletionSource gate;
            lock (_gateLock)
            {
                if (!_gates.TryGetValue(name, out gate!)) _gates[name] = gate = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
                if (!_waiting.TryGetValue(name, out var list)) _waiting[name] = list = new List<Id>();
                list.Add(job);
            }
            try
            {
                var cancelled = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
                using var registration = cancel.OnCancel(() => cancelled.TrySetResult());
                await Task.WhenAny(gate.Task, cancelled.Task);
                if (!gate.Task.IsCompleted) throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.JobCancelled)));
            }
            finally
            {
                lock (_gateLock) _waiting[name].Remove(job);
            }
        }

        public void Open(string name)
        {
            lock (_gateLock)
            {
                _opened.Add(name);
                if (!_gates.TryGetValue(name, out var gate)) _gates[name] = gate = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
                gate.TrySetResult();
            }
        }

        string NameOf(string id) => Jobs.First(j => j.Value.Value == id).Key;

        /// <summary>The engine has settled: everything still going on is parked (a step at a closed gate, a step waiting
        /// for an answer), and every job that was run has finished or waits (for resources, an answer, a gate).</summary>
        bool Settled()
        {
            bool ok = true;
            using var checkedIt = new ManualResetEventSlim();
            // Posted alone (no barrier after it), so the executor's activity counts only the engine's own work.
            W.Executor.Post(() =>
            {
                int parked = 0;
                var atGate = new HashSet<Id>();
                lock (_gateLock)
                    foreach (var (gate, jobs) in _waiting)
                        if (!_opened.Contains(gate))
                            foreach (var j in jobs.Where(j => !_cancelled.Contains(j)))
                            {
                                parked++;
                                atGate.Add(j);
                            }
                foreach (var (name, id) in Jobs)
                {
                    if (!_started.Contains(name) || Outcomes.ContainsKey(name)) continue;
                    var last = Events.LastOrDefault(e => e.Type is EngineEventType.JobChanged or EngineEventType.JobFinished
                                                         && e.Data["job"]!["id"]!.AsString == id.Value);
                    var state = last?.Data["job"]!["state"]!.AsString;
                    if (state == "awaitingDecision" && !_cancelled.Contains(id)) parked++;
                    else if (!(state is "waitingForResources" or "blocked" || atGate.Contains(id))) ok = false;
                }
                if (W.Executor.Activity != parked) ok = false;
                checkedIt.Set();
            });
            if (!checkedIt.Wait(Patience)) throw new TimeoutException("the executor didn't run the check");
            return ok;
        }

        public void Settle()
        {
            var until = DateTime.UtcNow + Patience;
            while (!Settled())
            {
                if (DateTime.UtcNow > until) throw new TimeoutException("the jobs didn't settle");
                Thread.Sleep(1);
            }
        }

        async void Track(string name, Task<Decided> run)
        {
            var decided = await run;
            Outcomes[name] = decided;
        }

        public void Run(JsonValue op)
        {
            var (name, arg) = op.AsObject!.Single();
            switch (name)
            {
                case "run":
                {
                    var job = arg.AsString!;
                    _started.Add(job);
                    W.OnExecutor(() => Track(job, Runner.Run(Jobs[job])));
                    break;
                }
                case "open":
                    Open(arg.AsString!);
                    break;
                case "cancel":
                    lock (_gateLock) _cancelled.Add(Jobs[arg.AsString!]);
                    W.OnExecutor(() => Runner.Cancel(Jobs[arg.AsString!]));
                    break;
                case "decide":
                {
                    BroError? error = null;
                    W.OnExecutor(() =>
                    {
                        try { Runner.Decide(Jobs[arg["job"]!.AsString!], arg["answer"]!.AsString!); }
                        catch (BroFailure f) { error = f.Error; }
                    });
                    Same(arg["expectError"]?.AsString, error?.Code, "decide's error");
                    break;
                }
                case "advance":
                    W.Clock.Advance(arg.AsInteger!.Value);
                    break;
                case "config":
                    W.OnExecutor(() => Config.Document = arg);
                    break;
                case "library":
                {
                    var id = arg["id"]!.AsString!;
                    W.OnExecutor(() => Scheduler.SetLibraryOffline(id, arg["online"]!.AsBool == true ? null
                        : new BroError(MessageCode.Wire(MessageCode.LibraryOffline), ("library", JsonValue.Of(id)))));
                    break;
                }
                default:
                    throw new InvalidOperationException("unknown op " + name);
            }
            Settle();
        }

        /// <summary>What was published about each job, as the case writes it.</summary>
        public Dictionary<string, List<string>> Lines()
        {
            var lines = Jobs.Keys.ToDictionary(k => k, _ => new List<string>());
            foreach (var e in Events)
            {
                switch (e.Type)
                {
                    case EngineEventType.JobChanged:
                    {
                        var job = e.Data["job"]!;
                        var text = $"changed {job["state"]!.AsString} {job["progress"]?["step"]?.AsString ?? "-"}";
                        if (job["progress"]?["currentFraction"]?.AsNumber is { } f) text += $" progress {(int)Math.Round(f * 100)}";
                        if (job["blockedBy"]?["code"]?.AsString is { } b) text += " blockedBy " + b;
                        if (job["decision"]?["id"]?.AsString is { } d) text += " decision " + d;
                        lines[NameOf(job["id"]!.AsString!)].Add(text);
                        break;
                    }
                    case EngineEventType.StepFinished:
                        lines[NameOf(e.Data["job"]!.AsString!)].Add($"step {e.Data["kind"]!.AsString} {e.Data["state"]!.AsString}");
                        break;
                    case EngineEventType.JobLog:
                        foreach (var l in e.Data["lines"]!.AsArray!) lines[NameOf(e.Data["jobId"]!.AsString!)].Add("log " + l["code"]!.AsString);
                        break;
                    case EngineEventType.JobFinished:
                    {
                        var job = e.Data["job"]!;
                        var text = "finished " + job["outcome"]!.AsString;
                        if (job["error"]?["code"]?.AsString is { } code) text += " " + code;
                        lines[NameOf(job["id"]!.AsString!)].Add(text);
                        break;
                    }
                }
            }
            return lines;
        }

        public List<string> QueueLines() => Events.Where(e => e.Type == EngineEventType.QueueChanged)
            .Select(e => e.Data["queue"]!.AsString + ": " + string.Join(" ", e.Data["order"]!.AsArray!.Select(id => NameOf(id.AsString!)))).ToList();

        public void Dispose() => W.Dispose();
    }

    static void SameLines(JsonValue want, IEnumerable<string> got, string what) =>
        Same(string.Join(" | ", Strings(want)), string.Join(" | ", got), what);

    [Fact]
    public void TheSharedCasesPass()
    {
        RunCases("engine/pipeline-runner.cases.json", (id, given, expect) =>
        {
            using var h = new Harness(given);
            foreach (var op in given["ops"]!.AsArray!) h.Run(op);
            foreach (var (name, want) in expect["outcomes"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
            {
                Same(want["outcome"]!.AsString, h.Outcomes.TryGetValue(name, out var d) ? EnumWire.Name(d.Outcome) : null, "outcome of " + name);
                if (want["error"] is { } e)
                {
                    Same(e["code"]!.AsString, d!.Error?.Code, "error of " + name);
                    if (e["params"] is { } p) Same<JsonValue>(p, d.Error!.Params, "error params of " + name);
                }
                else Same<string?>(null, d!.Error?.Code, "error of " + name);
            }
            var lines = h.Lines();
            foreach (var (name, want) in expect["lines"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>()) SameLines(want, lines[name], "lines of " + name);
            if (expect["queueLines"] is { } q) SameLines(q, h.QueueLines(), "queue lines");
            foreach (var (name, want) in expect["steps"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
                SameLines(want, h.Store.StepsOf(h.Jobs[name]).Select(s => $"{EnumWire.Name(s.Kind)} {EnumWire.Name(s.State)}"), "steps of " + name);
            foreach (var (name, want) in expect["records"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
            {
                var r = h.Store.Job(h.Jobs[name])!;
                Same(want["state"]!.AsString, EnumWire.Name(r.State), "record state of " + name);
                Same(want["outcome"]!.AsString, r.Outcome is { } o ? EnumWire.Name(o) : null, "record outcome of " + name);
                SameLines(want["plan"]!, r.Plan!.Steps.Select(k => EnumWire.Name(k)), "record plan of " + name);
            }
            foreach (var (name, want) in expect["logFile"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
            {
                var text = Encoding.UTF8.GetString(h.Fs.Read($"/data/jobs/{h.Jobs[name].Value}/log.jsonl"));
                var codes = text.Split('\n', StringSplitOptions.RemoveEmptyEntries).Select(l =>
                {
                    Assert.DoesNotContain("seq", l);
                    return JsonValue.Parse(l)!["code"]!.AsString!;
                });
                SameLines(want, codes, "log file of " + name);
            }
            foreach (var (name, want) in expect["outputs"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
                foreach (var (kind, data) in want.AsObject!)
                    Same(data, h.Store.StepsOf(h.Jobs[name]).Last(s => EnumWire.Name(s.Kind) == kind).Output!.Data, $"output of {name}'s {kind}");
            if (expect["events"] is { } events)
            {
                JsonValue? got = null;
                h.W.OnExecutor(() => got = JsonValue.Of(h.Events.Select(EngineEvent.Json)));
                Same(events, got!, "events");
            }
            return true;
        });
    }

    [Fact]
    public void TheSharedPlannerCasesPass()
    {
        RunCases("engine/job-planner.cases.json", (id, given, expect) =>
        {
            var r = given["request"]!;
            var request = new JobRequest(EnumWire.Parse<JobKind>(r["kind"]!.AsString)!.Value, r["title"]!.AsString!, r["automatic"]?.AsBool == true,
                r["driveId"]?.AsString, SessionId: r["sessionId"]?.AsString is { } s ? new Id(s) : null);
            SameLines(expect["steps"]!, JobPlanner.Initial(request).Select(k => EnumWire.Name(k)), "steps");
            return true;
        });
    }
}
