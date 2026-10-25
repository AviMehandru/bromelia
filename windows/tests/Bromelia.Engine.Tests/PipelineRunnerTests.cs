using System.Text;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Engine.Tests;

/// <summary>shared/fixtures/engine/pipeline-runner.cases.json, job-service.cases.json (the same harness with a JobService
/// over the runner) and job-planner.cases.json.</summary>
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

        public FakeStep(Harness h, StepKind kind)
        {
            _h = h;
            Kind = kind;
        }

        JsonValue _b => _h.Behaviour(Kind);

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
            if (context.Finishing is { } end)
            {
                foreach (var code in Strings(_b["log"])) context.Log.Log(new BroMessage(MessageCode.Parse(code)!.Value));
                lock (_h.Finishing) _h.Finishing[context.Job.Id] = EnumWire.Name(end.Outcome) + " " + end.Error?.Code;
                return new StepReport(EmptyObject);
            }
            foreach (var p in _b["progress"]?.AsArray ?? new List<JsonValue>())
                context.Progress.Report(p.AsArray![0].AsNumber!.Value, new BroMessage(MessageCode.Parse(p.AsArray[1].AsString!)!.Value));
            foreach (var code in Strings(_b["log"])) context.Log.Log(new BroMessage(MessageCode.Parse(code)!.Value));
            var data = _b["output"] ?? EmptyObject;
            if (_b["config"]?.AsString is { } key)
                data = JsonValue.Of((key, context.Config.Document[key] ?? JsonValue.Null.Instance));
            if (_b["echo"]?.AsString is { } echo)
                data = context.Job.Outputs.Last(o => EnumWire.Name(o.Kind) == echo).Data;
            if (_b["context"]?.AsBool == true)
                data = JsonValue.Of(("startedAt", context.Job.StartedAt is { } started ? JsonValue.Of(Instant.Format(started)) : JsonValue.Null.Instance),
                    ("appVersion", JsonValue.Of(context.AppVersion)), ("message", JsonValue.Of(context.Messages.Render(new BroMessage(MessageCode.JobCancelled)))));
            if (_b["ask"] is { } ask)
            {
                var question = new DecisionDto(ask["id"]!.AsString!, Message(ask["question"]!),
                    Strings(ask["answers"]).Select(a => new DecisionAnswer(a, new BroMessage(MessageCode.Parse("decision.answer." + a)!.Value))).ToList());
                var answer = await context.Decisions.Ask(question);
                data = JsonValue.Of(("answer", JsonValue.Of(answer)));
            }
            if (_b["checkpoint"] is { } checkpoint) await context.Checkpoint.Save(checkpoint);
            if (_b["saved"]?.AsBool == true) data = JsonValue.Of(("saved", context.Checkpoint.Saved ?? JsonValue.Null.Instance));
            if (_b["reserve"] is { } r)
                await context.Space.Reserve(r["volume"]!.AsString!, r["path"]!.AsString!, r["need"]!.AsInteger!.Value, r["free"]!.AsInteger!.Value);
            if (_b["gate"]?.AsString is { } gate) await _h.Gate(gate, context.Job.Id, context.Cancel, _b["ignoresCancel"]?.AsBool == true);
            return new StepReport(
                data,
                AppendSteps: Strings(_b["append"]).Select(k => EnumWire.Parse<StepKind>(k)!.Value).ToList(),
                Skipped: _b["skipped"]?.AsBool == true,
                ReadErrors: (int)(_b["readErrors"]?.AsInteger ?? 0),
                Quarantined: _b["quarantined"]?.AsBool == true,
                Notice: Notice(_b["notice"]?.AsString),
                SkipJob: _b["skipJob"] is { } skip ? Message(skip, Severity.Warning) : null,
                Error: _b["fail"] is { } fail ? Error(fail) : null,
                AffectsOutcome: _b["affectsOutcome"]?.AsBool ?? true,
                Plan: _b["plan"] is { } plan
                    ? new JobPlan(new List<StepKind>(), plan["mode"]?.AsString, plan["profileId"]?.AsString, plan["libraryId"]?.AsString)
                    : null);
        }
    }

    /// <summary>Ids that count: 00000000-0000-4000-8000-000000000001, … after the given jobs'.</summary>
    internal sealed class CountingIds : IIdGenerator
    {
        long _n;

        public CountingIds(long given) { _n = given; }

        public Id Next() => new($"00000000-0000-4000-8000-{Interlocked.Increment(ref _n):x12}");

        public Id Peek() => new($"00000000-0000-4000-8000-{Interlocked.Read(ref _n) + 1:x12}");
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
        /// <summary>What the Release at the end saw, by job: '&lt;outcome&gt; &lt;error code&gt;'.</summary>
        public readonly Dictionary<Id, string> Finishing = new();
        public readonly List<EngineEvent> Events = new();
        readonly object _gateLock = new();
        readonly Dictionary<string, TaskCompletionSource> _gates = new();
        readonly Dictionary<Id, int> _atGate = new();

        readonly bool _service;

        /// <param name="service">job-service cases: cancel and decide go through the JobService.</param>
        public Harness(JsonValue given, bool service = false)
        {
            _service = service;
            Bus = new EventBus(W.Clock);
            var limits = given["limits"];
            Scheduler = new Scheduler(W.Executor, Bus, new SchedulerLimits(Acquisitions: (int)(limits?["acquisitions"]?.AsInteger ?? 0)));
            if (given["config"] is { } config) Config.Document = config;
            var steps = new List<IStep>();
            foreach (var kind in Enum.GetValues<StepKind>())
            {
                _behaviour[kind] = given["steps"]?[EnumWire.Name(kind)] ?? EmptyObject;
                steps.Add(new FakeStep(this, kind));
            }
            var env = new StepEnvironment(new ToolSet(null!, null!), Fs, W.Clock, null!, null!, null!, Config, null!, null!, "/data",
                new RecoveryServiceTests.Renderer(), "test");
            Runner = new PipelineRunner(W.Executor, Bus, Scheduler, Store, new StepRegistry(steps), env, W.EngineClock);
            Ids = new CountingIds(given["jobs"]!.AsArray!.Count);
            Service = new JobService(W.Executor, Bus, Store, Scheduler, Runner, W.EngineClock, Ids, Fs, "/data");
            W.OnExecutor(() => Bus.Subscribe(null, OnEvent));
            int n = 0;
            foreach (var j in given["jobs"]!.AsArray!)
            {
                var id = new Id($"00000000-0000-4000-8000-{++n:x12}");
                var name = j["name"]!.AsString!;
                Jobs[name] = id;
                var kind = EnumWire.Parse<JobKind>(j["kind"]?.AsString ?? "videoDisc")!.Value;
                var title = j["title"]?.AsString ?? name;
                var request = new JobRequest(kind, title, j["automatic"]?.AsBool == true, j["driveId"]?.AsString, j["mediaGeneration"]?.AsInteger);
                var plan = j["plan"] is { } p ? new JobPlan(Strings(p).Select(k => EnumWire.Parse<StepKind>(k)!.Value).ToList()) : null;
                var queue = EnumWire.Parse<Queue>(j["queue"]?.AsString ?? "acquisition")!.Value;
                var jobState = EnumWire.Parse<JobState>(j["state"]?.AsString ?? "queued")!.Value;
                var created = j["created"]?.AsString is { } c ? Instant.Parse(c)!.Value : W.Clock.Now();
                var record = new JobRecord(id, kind, null, jobState, EnumWire.Parse<Outcome>(j["outcome"]?.AsString), null, null, null, queue,
                    (int)(j["position"]?.AsInteger ?? 0), request.DriveId, request.MediaGeneration, request.Automatic, null, title, null, request, plan,
                    null, created, FinishedAt: jobState == JobState.Finished ? W.Clock.Now() : null);
                ((IJobRepository)Store).Insert(record).Wait();
                int seq = 0;
                foreach (var d in j["done"]?.AsArray ?? new List<JsonValue>())
                {
                    var k = EnumWire.Parse<StepKind>(d["kind"]!.AsString)!.Value;
                    var state = EnumWire.Parse<StepState>(d["state"]?.AsString ?? "succeeded")!.Value;
                    ((IStepRepository)Store).Save(new StepRecord(id, seq++, k, state, 1, W.Clock.Now(), W.Clock.Now(),
                        new StepOutput(k, d["output"] ?? EmptyObject, null, (int)(d["readErrors"]?.AsInteger ?? 0)), null, d["checkpoint"])).Wait();
                }
                if (j["intent"]?.AsBool == true) Store.OpenIntent(id);
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

        public readonly JobService Service;
        public readonly CountingIds Ids;
        public readonly List<JsonValue> Queries = new();
        public readonly List<JsonValue> Logs = new();
        public readonly List<string> AnnouncedUnstored = new();
        readonly Dictionary<StepKind, JsonValue> _behaviour = new();

        public JsonValue Behaviour(StepKind kind)
        {
            lock (_behaviour) return _behaviour[kind];
        }

        /// <summary>Every event; a jobFinished gives the job's outcome, a jobChanged must come after its record is stored.</summary>
        void OnEvent(EngineEvent e)
        {
            Events.Add(e);
            if (e.Type is not (EngineEventType.JobChanged or EngineEventType.JobFinished)) return;
            var job = e.Data["job"]!;
            var id = new Id(job["id"]!.AsString!);
            if (e.Type == EngineEventType.JobChanged && Store.Job(id) == null) AnnouncedUnstored.Add(id.Value);
            if (e.Type == EngineEventType.JobFinished && Jobs.FirstOrDefault(j => j.Value == id).Key is { } name)
                Outcomes[name] = new Decided(EnumWire.Parse<Outcome>(job["outcome"]!.AsString)!.Value, job["error"] is { } err ? Error(err) : null);
        }

        readonly Dictionary<string, List<Id>> _waiting = new(); // gate → jobs waiting there
        readonly HashSet<string> _opened = new();
        readonly HashSet<Id> _cancelled = new();
        readonly HashSet<Id> _stubborn = new(); // at a gate that ignores the cancel
        readonly HashSet<string> _started = new();

        public async Task Gate(string name, Id job, Foundation.CancellationToken cancel, bool ignoresCancel = false)
        {
            TaskCompletionSource gate;
            lock (_gateLock)
            {
                if (ignoresCancel) _stubborn.Add(job);
                if (!_gates.TryGetValue(name, out gate!)) _gates[name] = gate = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
                if (!_waiting.TryGetValue(name, out var list)) _waiting[name] = list = new List<Id>();
                list.Add(job);
            }
            try
            {
                var cancelled = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
                using var registration = cancel.OnCancel(() =>
                {
                    if (!ignoresCancel) cancelled.TrySetResult();
                });
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
                            foreach (var j in jobs.Where(j => !_cancelled.Contains(j) || _stubborn.Contains(j)))
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
                    if (state == null && _service) continue; // a created job not stored yet (its insert held): the activity tells
                    if (state == "awaitingDecision" && !_cancelled.Contains(id)) parked++;
                    else if (!(state is "waitingForResources" or "blocked" || atGate.Contains(id))) ok = false;
                }
                if (W.Executor.Activity != parked + Store.HeldInserts + Store.HeldLoads) ok = false;
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
                case "cancel" when !_service:
                    lock (_gateLock) _cancelled.Add(Jobs[arg.AsString!]);
                    W.OnExecutor(() => Runner.Cancel(Jobs[arg.AsString!]));
                    break;
                case "decide" when !_service:
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
                case "holdInserts":
                    Store.HoldInserts();
                    break;
                case "releaseInserts":
                    Store.ReleaseInserts();
                    break;
                case "holdLoads":
                    Store.HoldLoads(Jobs[arg.AsString!]);
                    break;
                case "releaseLoads":
                    Store.ReleaseLoads();
                    break;
                default:
                {
                    if (!_service) throw new InvalidOperationException("unknown op " + name);
                    var call = ServiceCall(op);
                    Task? task = null;
                    W.OnExecutor(() => task = call());
                    _calls.Add((op, task!));
                    break;
                }
            }
            Settle();
        }

        Id IdOf(JsonValue job) => Jobs.TryGetValue(job.AsString!, out var id) ? id : new Id("00000000-0000-4000-8000-0000000000ff");

        static JsonValue Arg(JsonValue op) => op.AsObject!.Single().Value;

        /// <summary>A JobService op: what it calls on the executor. A job it creates gets its name now (the ids count).</summary>
        Func<Task> ServiceCall(JsonValue op)
        {
            var (name, arg) = op.AsObject!.Single();
            var job = arg.AsString != null ? arg : arg["job"];
            switch (name)
            {
                case "start":
                    foreach (var (n, id) in Jobs)
                        if (Store.Job(id) is { State: not JobState.Finished }) _started.Add(n);
                    return () => Service.Start();
                case "create":
                {
                    var r = arg;
                    var title = r["title"]?.AsString ?? r["as"]!.AsString!;
                    var request = new JobRequest(EnumWire.Parse<JobKind>(r["kind"]?.AsString ?? "videoDisc")!.Value, title, r["automatic"]?.AsBool == true,
                        r["driveId"]?.AsString, r["mediaGeneration"]?.AsInteger);
                    Name(r["as"]!.AsString!);
                    return () => Service.Create(request);
                }
                case "retry":
                {
                    var id = IdOf(job!);
                    if (arg["as"]?.AsString is { } a) Name(a);
                    var from = arg["fromStep"]?.AsString is { } f ? EnumWire.Parse<StepKind>(f) : null;
                    return () => Service.Retry(id, from);
                }
                case "cancel":
                {
                    var id = IdOf(job!);
                    lock (_gateLock) _cancelled.Add(id);
                    return () => Service.Cancel(id);
                }
                case "decide":
                {
                    var id = IdOf(job!);
                    return () => Service.Decide(id, arg["decisionId"]!.AsString!, arg["answer"]!.AsString!);
                }
                case "move":
                {
                    var id = IdOf(job!);
                    return () => Service.Move(id, (int)arg["by"]!.AsInteger!.Value);
                }
                case "startNow":
                {
                    var id = IdOf(job!);
                    return () => Service.StartNow(id);
                }
                case "steps":
                    lock (_behaviour)
                        foreach (var (kind, behaviour) in arg.AsObject!) _behaviour[EnumWire.Parse<StepKind>(kind)!.Value] = behaviour;
                    return () => Task.CompletedTask;
                case "query":
                {
                    var q = new JobQueryDto(
                        Strings(arg["states"]).Select(x => EnumWire.Parse<JobState>(x)!.Value).ToList(),
                        Strings(arg["outcomes"]).Select(x => EnumWire.Parse<Outcome>(x)!.Value).ToList(),
                        EnumWire.Parse<JobKind>(arg["kind"]?.AsString), EnumWire.Parse<Queue>(arg["queue"]?.AsString), arg["driveId"]?.AsString, null,
                        arg["cursor"]?.AsString, (int)(arg["limit"]?.AsInteger ?? JobQueryDto.DefaultLimit));
                    return async () =>
                    {
                        try
                        {
                            var page = await Service.Query(q);
                            Queries.Add(JsonValue.Of(("items", JsonValue.Of(page.Items.Select(i => JsonValue.Of(ItemText(i))))),
                                ("next", JsonValue.Of(page.Next)), ("total", JsonValue.Of(page.Total))));
                        }
                        catch (BroFailure f)
                        {
                            Queries.Add(JsonValue.Of(("error", JsonValue.Of(f.Error.Code))));
                            throw;
                        }
                    };
                }
                case "log":
                {
                    var id = IdOf(job!);
                    return async () =>
                    {
                        try
                        {
                            var lines = await Service.Log(id, arg["after"]!.AsInteger!.Value);
                            Logs.Add(JsonValue.Of(lines.Select(l => JsonValue.Of($"{l.Seq} {MessageCode.Wire(l.Code)}"))));
                        }
                        catch (BroFailure f)
                        {
                            Logs.Add(JsonValue.Of(("error", JsonValue.Of(f.Error.Code))));
                            throw;
                        }
                    };
                }
                default:
                    throw new InvalidOperationException("unknown op " + name);
            }
        }

        void Name(string name)
        {
            Jobs[name] = Ids.Peek();
            _started.Add(name);
        }

        string ItemText(JobDto j)
        {
            var text = $"{NameOf(j.Id.Value)} {EnumWire.Name(j.State)}";
            if (j.Outcome is { } o) text += " " + EnumWire.Name(o);
            if (j.Position is { } p) text += " position " + p;
            if (j.Progress is { } progress) text += " step " + EnumWire.Name(progress.Step);
            return text;
        }

        readonly List<(JsonValue Op, Task Task)> _calls = new();

        /// <summary>Every JobService op has ended as it expects (its expectError, else no error).</summary>
        public void CheckCalls()
        {
            foreach (var (op, task) in _calls)
            {
                Assert.True(task.IsCompleted, "the op hasn't finished: " + op);
                var error = task.Exception?.InnerException is BroFailure f ? f.Error.Code : task.Exception?.InnerException?.ToString();
                Same(Arg(op).AsObject != null ? Arg(op)["expectError"]?.AsString : null, error, "error of " + op);
            }
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
                        if (job["position"]?.AsInteger is { } position) text += " position " + position;
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
            Expect(h, expect);
            return true;
        });
    }

    [Fact]
    public void TheSharedJobServiceCasesPass()
    {
        RunCases("engine/job-service.cases.json", (id, given, expect) =>
        {
            using var h = new Harness(given, service: true);
            foreach (var op in given["ops"]!.AsArray!) h.Run(op);
            h.CheckCalls();
            Expect(h, expect);
            Same("", string.Join(" ", h.AnnouncedUnstored), "jobs announced before they were stored");
            if (expect["queries"] is { } queries) Same(queries, JsonValue.Of(h.Queries), "queries");
            if (expect["logs"] is { } logs) Same(logs, JsonValue.Of(h.Logs), "logs");
            return true;
        });
    }

    /// <summary>The expectations both case files share.</summary>
    static void Expect(Harness h, JsonValue expect)
    {
        {
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
                var got = new Dictionary<string, JsonValue>
                {
                    ["state"] = JsonValue.Of(EnumWire.Name(r.State)),
                    ["outcome"] = JsonValue.Of(r.Outcome is { } o ? EnumWire.Name(o) : null),
                    ["queue"] = JsonValue.Of(EnumWire.Name(r.Queue)),
                    ["position"] = JsonValue.Of(r.Position),
                    ["automatic"] = JsonValue.Of(r.Automatic),
                    ["mediaGeneration"] = r.MediaGeneration is { } g ? JsonValue.Of(g) : JsonValue.Null.Instance,
                    ["title"] = JsonValue.Of(r.Title),
                    ["driveId"] = JsonValue.Of(r.DriveId),
                    ["plan"] = r.Plan is { } plan ? JsonValue.Of(plan.Steps.Select(k => JsonValue.Of(EnumWire.Name(k)))) : JsonValue.Null.Instance,
                    ["mode"] = JsonValue.Of(r.Mode),
                    ["libraryId"] = JsonValue.Of(r.Plan?.LibraryId),
                };
                foreach (var (key, value) in want.AsObject!) Same(value, got[key], $"record {key} of {name}");
            }
            foreach (var (name, want) in expect["finishing"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
                lock (h.Finishing) Same(want, JsonValue.Of(h.Finishing.TryGetValue(h.Jobs[name], out var seen) ? seen : null), "finishing of " + name);
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
            foreach (var (name, want) in expect["checkpoints"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
                foreach (var (kind, checkpoint) in want.AsObject!)
                    Same(checkpoint, h.Store.StepsOf(h.Jobs[name]).Last(s => EnumWire.Name(s.Kind) == kind).Checkpoint ?? JsonValue.Null.Instance,
                        $"checkpoint of {name}'s {kind}");
            foreach (var (name, want) in expect["readErrors"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
                foreach (var (kind, count) in want.AsObject!)
                    Same(count.AsInteger, (long?)h.Store.StepsOf(h.Jobs[name]).Last(s => EnumWire.Name(s.Kind) == kind).Output!.ReadErrors,
                        $"read errors of {name}'s {kind}");
            foreach (var (name, want) in expect["outputs"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
                foreach (var (kind, data) in want.AsObject!)
                    Same(data, h.Store.StepsOf(h.Jobs[name]).Last(s => EnumWire.Name(s.Kind) == kind).Output!.Data, $"output of {name}'s {kind}");
            if (expect["events"] is { } events)
            {
                JsonValue? got = null;
                h.W.OnExecutor(() => got = JsonValue.Of(h.Events.Select(EngineEvent.Json)));
                Same(events, got!, "events");
            }
        }
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
            if (expect["queue"] is { } queue) Same(queue.AsString, EnumWire.Name(JobPlanner.Queue(request.Kind)), "queue");
            return true;
        });
    }
}
