using System.Text;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Engine.Tests;

/// <summary>shared/fixtures/engine/recovery-service.cases.json, on a real executor, event bus, problem service, scheduler and
/// library service, with the library harness's file tree and database, and jobs and steps in memory.</summary>
public sealed class RecoveryServiceTests
{
    static readonly TimeSpan Patience = LibraryServiceTests.Patience;
    static readonly JsonValue.Object NoParams = new(new List<KeyValuePair<string, JsonValue>>());

    /// <summary>Jobs and steps (MemoryStore, jobs in the order given) with the library harness's units and libraries.</summary>
    sealed class Store : IStore
    {
        public readonly MemoryStore Jobs_ = new();
        public readonly LibraryServiceTests.Db Db = new();
        public IJobRepository Jobs() => Jobs_.Jobs();
        public IStepRepository Steps() => Jobs_.Steps();
        public IUnitRepository Units() => Db;
        public ILibraryRepository Libraries() => Db;
        public ICatalogRepository Catalog() => throw new NotSupportedException();
        public ICheckRepository Checks() => throw new NotSupportedException();
        public IReplicaRepository Replicas() => throw new NotSupportedException();
        public IDriveRepository Drives() => throw new NotSupportedException();
        public ILookupCacheRepository LookupCache() => throw new NotSupportedException();
        public IOutboxRepository Outbox() => throw new NotSupportedException();
        public IKeyValueRepository Kv() => throw new NotSupportedException();
        public Task<T> Transaction<T>(Func<IStore, Task<T>> block) => block(this);
        public Task Migrate() => Task.CompletedTask;
    }

    /// <summary>A step that only says what it can do after a stop; a resumeFrom one resumes from a "backup" checkpoint.</summary>
    sealed class FakeStep : IStep
    {
        public FakeStep(StepKind kind, Resumability resumability)
        {
            Kind = kind;
            Resumability = resumability;
        }

        public StepKind Kind { get; }
        public Resumability Resumability { get; }
        public string? ResumeCheckpoint => Resumability == Resumability.ResumeFrom ? "backup" : null;
        public ResourceSet Resources(JobSnapshot job) => ResourceSet.None;
        public Task<StepReport> Run(StepContext context) => throw new NotSupportedException();
    }

    sealed class Isolation : ISettingsIsolation
    {
        public readonly List<string> Log = new();
        public IReadOnlyList<BroMessage> Problems = Array.Empty<BroMessage>();
        public IIsolationLease Prepare(MakemkvRunSettings settings) => throw new NotSupportedException();

        public IReadOnlyList<BroMessage> Recover(IReadOnlyList<string> workDirectories)
        {
            lock (Log) Log.Add(string.Join(" ", new[] { "recover" }.Concat(workDirectories)));
            return Problems;
        }
    }

    /// <summary>'&lt;code&gt;' then ' &lt;key&gt;=&lt;value&gt;' per param (strings as they are, other values as JSON).</summary>
    sealed class Renderer : IMessageRenderer
    {
        public string Render(BroMessage message) => MessageCode.Wire(message.Code) + string.Concat(message.Params.Members.Select(m =>
            " " + m.Key + "=" + (m.Value.AsString ?? Encoding.UTF8.GetString(JsonValue.EncodeCanonical(m.Value)))));
    }

    static JsonValue? Error(BroError? e)
    {
        if (e == null) return null;
        var m = new List<KeyValuePair<string, JsonValue>> { new("code", JsonValue.Of(e.Code)) };
        if (e.Params.Members.Count > 0) m.Add(new("params", e.Params));
        return new JsonValue.Object(m);
    }

    static JsonValue Projection(JobRecord r)
    {
        var m = new List<KeyValuePair<string, JsonValue>>
        {
            new("state", JsonValue.Of(EnumWire.Name(r.State))),
            new("queue", JsonValue.Of(EnumWire.Name(r.Queue))),
            new("plan", r.Plan is { } p ? JsonValue.Of(p.Steps.Select(k => JsonValue.Of(EnumWire.Name(k)))) : JsonValue.Null.Instance),
            new("mediaGeneration", r.MediaGeneration is { } g ? JsonValue.Of(g) : JsonValue.Null.Instance),
        };
        if (r.Outcome is { } o) m.Add(new("outcome", JsonValue.Of(EnumWire.Name(o))));
        if (Error(r.Error) is { } e) m.Add(new("error", e));
        if (r.Decision is { } d) m.Add(new("decision", d));
        return new JsonValue.Object(m);
    }

    [Fact]
    public void TheSharedCasesPass()
    {
        RunCases("engine/recovery-service.cases.json", (id, given, expect) =>
        {
            using var w = new EngineClockTests.World();
            var tree = new LibraryServiceTests.Tree();
            foreach (var d in given["dirs"]?.AsArray ?? new List<JsonValue>()) tree.MakeDirs(d.AsString!);
            foreach (var (p, t) in given["files"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>()) tree.Put(p, t.AsString!);
            foreach (var h in given["hold"]?.AsArray ?? new List<JsonValue>()) tree.Holds.Add((h["op"]!.AsString!, h["path"]!.AsString!));
            var store = new Store();
            var db = store.Db;
            foreach (var l in given["libraries"]?.AsArray ?? new List<JsonValue>())
                db.Libraries[l["id"]!.AsString!] = new LibraryRecord(l["id"]!.AsString!, l["name"]!.AsString!, l["path"]!.AsString!, l["markerId"]!.AsString!,
                    Instant.Parse(l["attachedAt"]!.AsString!)!.Value, l["online"]!.AsBool!.Value, l["lastSeenAt"]?.AsString is { } s ? Instant.Parse(s) : null);
            foreach (var u in given["units"]?.AsArray ?? new List<JsonValue>())
                db.Units[u["id"]!.AsString!] = new UnitRecord(new Id(u["id"]!.AsString!), u["library"]!.AsString!, null, new Id(u["job"]!.AsString!),
                    u["path"]!.AsString!, "bromelia-00000000.json", 3, UnitState.Committing, UnitStatus.Success, u["name"]!.AsString!, MediaKind.Movie,
                    "bluray", "BD", false, null, "", null, null, null, null, "", 0, 0, 1, w.Clock.Now());
            foreach (var i in given["intents"]?.AsArray ?? new List<JsonValue>())
                db.Intents[new Id(i["unit"]!.AsString!)] = new CommitIntent(new Id(i["unit"]!.AsString!), new Id(i["job"]!.AsString!), i["staging"]!.AsString!,
                    i["destination"]!.AsString!, i["merge"]!.AsBool!.Value, false, w.Clock.Now(), i["items"]!.AsArray!.Select(x => new CommitItem(
                        (int)x.AsArray![0].AsInteger!.Value, x.AsArray![1].AsString!, x.AsArray![2].AsString!, x.AsArray![3].AsString, x.AsArray![4].AsBool!.Value,
                        x.AsArray![5].AsBool!.Value)).ToList());
            var jobs = (IJobRepository)store.Jobs_;
            var steps = (IStepRepository)store.Jobs_;
            foreach (var j in given["jobs"]?.AsArray ?? new List<JsonValue>())
            {
                var jobId = new Id(j["id"]!.AsString!);
                var generation = j["mediaGeneration"]?.AsInteger;
                var request = new JobRequest(JobKind.VideoDisc, j["title"]!.AsString!, j["automatic"]?.AsBool == true, j["driveId"]?.AsString, generation);
                var plan = j["plan"] is JsonValue.Array a ? new JobPlan(a.Items.Select(k => EnumWire.Parse<StepKind>(k.AsString)!.Value).ToList()) : null;
                jobs.Insert(new JobRecord(jobId, JobKind.VideoDisc, null, EnumWire.Parse<JobState>(j["state"]!.AsString)!.Value,
                    j["outcome"]?.AsString is { } o ? EnumWire.Parse<Outcome>(o) : null, j["error"] is { } e ? LibraryServiceTests.Failure(e).Error : null, null,
                    j["decision"], EnumWire.Parse<Queue>(j["queue"]?.AsString ?? "acquisition")!.Value, 0, request.DriveId, generation, request.Automatic, null,
                    request.Title, null, request, plan, j["unitId"]?.AsString is { } u ? new Id(u) : null, w.Clock.Now())).Wait();
                var seq = 0;
                foreach (var r in j["rows"]!.AsArray!)
                {
                    var kind = EnumWire.Parse<StepKind>(r["kind"]!.AsString)!.Value;
                    steps.Save(new StepRecord(jobId, seq++, kind, EnumWire.Parse<StepState>(r["state"]!.AsString)!.Value, 1, w.Clock.Now(), null,
                        new StepOutput(kind, JsonValue.Of(), null, (int)(r["readErrors"]?.AsInteger ?? 0)), r["error"] is { } re ? LibraryServiceTests.Failure(re).Error : null, r["checkpoint"])).Wait();
                }
            }
            var resumability = given["resumability"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>();
            var registry = new StepRegistry(Enum.GetValues<StepKind>().Select(k => new FakeStep(k,
                resumability.FirstOrDefault(m => m.Key == EnumWire.Name(k)).Value?.AsString is { } r ? EnumWire.Parse<Resumability>(r)!.Value : Resumability.Idempotent)));
            var isolation = new Isolation
            {
                Problems = (given["isolationProblems"]?.AsArray ?? new List<JsonValue>()).Select(BroMessage.Decode).ToList(),
            };
            var config = new LibraryServiceTests.ConfigBox { Value = LibraryServiceTests.ConfigBox.Of(given["config"] ?? JsonValue.Of(Array.Empty<JsonValue>())) };

            var events = new List<EngineEvent>();
            var bus = new EventBus(w.Clock);
            var problems = new ProblemService(bus, w.Clock);
            var scheduler = new Scheduler(w.Executor, bus, new SchedulerLimits());
            var library = new LibraryService(w.Executor, bus, problems, scheduler, config, w.EngineClock, tree, db, db);
            var service = new RecoveryService(w.Executor, bus, problems, store, registry, library, config, w.EngineClock, tree, isolation, new Renderer(), "/data");
            w.OnExecutor(() => bus.Subscribe(new[] { EngineEventType.JobChanged, EngineEventType.JobFinished, EngineEventType.ProblemRaised }, events.Add));

            bool Settled()
            {
                var ok = false;
                var checkedIt = new ManualResetEventSlim();
                w.Executor.Post(() =>
                {
                    ok = w.Executor.Activity == Volatile.Read(ref tree.Held);
                    checkedIt.Set();
                });
                if (!checkedIt.Wait(Patience)) throw new TimeoutException("the executor didn't run the check");
                return ok;
            }
            void Settle()
            {
                var until = DateTime.UtcNow + Patience;
                while (!Settled())
                {
                    if (DateTime.UtcNow > until) throw new TimeoutException("the service didn't settle");
                    Thread.Sleep(1);
                }
            }
            w.Clock.Settle = Settle;

            var ops = given["ops"]!.AsArray!;
            var results = new JsonValue?[ops.Count];
            ManualResetEventSlim? recovered = null;
            for (int i = 0; i < ops.Count; i++)
            {
                var (name, arg) = ops[i] is JsonValue.String str ? new KeyValuePair<string, JsonValue>(str.Value, JsonValue.Of()) : ops[i].AsObject!.Single();
                var at = i;
                switch (name)
                {
                    case "recover":
                    {
                        var d = recovered = new ManualResetEventSlim();
                        w.Executor.Post(async () =>
                        {
                            try { results[at] = await service.Recover() is { } m ? m.ToJson() : JsonValue.Null.Instance; }
                            catch (BroFailure f) { results[at] = JsonValue.Of(("error", Error(f.Error)!)); }
                            catch (Exception e) { results[at] = JsonValue.Of(("unexpected", JsonValue.Of(e.GetType().Name + ": " + e.Message))); }
                            d.Set();
                        });
                        if (arg["wait"]?.AsBool != false && !d.Wait(Patience)) throw new TimeoutException("recover didn't finish");
                        Settle();
                        break;
                    }
                    case "recovered":
                        Settle();
                        results[i] = JsonValue.Of(recovered!.IsSet);
                        break;
                    case "advance":
                        w.Clock.Advance((long)(arg.AsNumber!.Value * 1000));
                        Settle();
                        results[i] = JsonValue.Of("ok");
                        break;
                    case "release":
                    {
                        tree.Gate.Set();
                        var until = DateTime.UtcNow + Patience;
                        while (Volatile.Read(ref tree.Held) > 0)
                        {
                            if (DateTime.UtcNow > until) throw new TimeoutException("the held calls didn't leave");
                            Thread.Sleep(1);
                        }
                        Settle();
                        results[i] = JsonValue.Of("ok");
                        break;
                    }
                    default:
                        throw new InvalidOperationException("unknown op " + name);
                }
            }
            if (recovered != null && !recovered.Wait(Patience)) throw new TimeoutException("recover didn't finish");
            Settle();

            Same(expect["results"]!, JsonValue.Of(results.Select(r => r ?? JsonValue.Null.Instance)), "results");
            foreach (var (jobId, want) in expect["jobs"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
                Same(want, Projection(store.Jobs_.Job(new Id(jobId))!), "job " + jobId);
            foreach (var (jobId, want) in expect["steps"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
                Same(want, JsonValue.Of(store.Jobs_.StepsOf(new Id(jobId)).Select(s =>
                    JsonValue.Of(EnumWire.Name(s.Kind) + " " + EnumWire.Name(s.State) + (s.Checkpoint != null ? " checkpoint" : "")))), "steps of " + jobId);
            foreach (var (jobId, want) in expect["logs"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
            {
                var path = $"/data/jobs/{jobId}/log.jsonl";
                var lines = tree.Files.TryGetValue(path, out var bytes)
                    ? Encoding.UTF8.GetString(bytes).Split('\n', StringSplitOptions.RemoveEmptyEntries).Select(l =>
                    {
                        Assert.False(l.Contains("\"seq\""), "a log line has seq: " + l);
                        var o = JsonValue.Parse(l)!;
                        var m = new List<KeyValuePair<string, JsonValue>> { new("severity", o["severity"]!), new("code", o["code"]!) };
                        if (o["params"] is JsonValue.Object { Members.Count: > 0 } p) m.Add(new("params", p));
                        return (JsonValue)new JsonValue.Object(m);
                    }).ToList()
                    : new List<JsonValue>();
                Same(want, JsonValue.Of(lines), "log of " + jobId);
            }
            JsonValue? got = null;
            w.OnExecutor(() => got = JsonValue.Of(
                ("problems", JsonValue.Of(problems.Problems().Select(ProblemDto.Json))),
                ("lines", JsonValue.Of(events.Select(e => JsonValue.Of(e.Type switch
                {
                    EngineEventType.JobChanged => $"jobChanged {e.Data["job"]!["id"]!.AsString} {e.Data["job"]!["state"]!.AsString}",
                    EngineEventType.JobFinished => $"jobFinished {e.Data["job"]!["id"]!.AsString} {e.Data["job"]!["outcome"]!.AsString}"
                        + (e.Data["job"]!["error"]?["code"]?.AsString is { } c ? " " + c : ""),
                    _ => $"problemRaised {e.Data["problem"]!["code"]!.AsString}",
                }))))));
            if (expect["eventLines"] is { } el) Same(el, got!["lines"]!, "event lines");
            if (expect["problems"] is { } pr) Same(pr, got!["problems"]!, "problems");
            if (expect["isolation"] is { } iso) Same(iso, JsonValue.Of(isolation.Log.Select(JsonValue.Of)), "isolation");
            if (expect["files"] is { } fl)
                Same(Sorted(fl), Sorted(new JsonValue.Object(tree.Files
                    .Where(f => !(f.Key.StartsWith("/data/jobs/", StringComparison.Ordinal) && f.Key.EndsWith("/" + JobLog.FileName, StringComparison.Ordinal)))
                    .Select(f => new KeyValuePair<string, JsonValue>(f.Key, JsonValue.Of(Encoding.UTF8.GetString(f.Value)))).ToList())), "files");
            foreach (var (folder, want) in expect["dirsUnder"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
                Same(want, JsonValue.Of(tree.Dirs.Where(d => d.StartsWith(folder + "/", StringComparison.Ordinal)).Select(JsonValue.Of)), "folders under " + folder);
            if (expect["db"] is { } wantDb)
            {
                if (wantDb["units"] is { } units)
                    Same(units, JsonValue.Of(db.Units.Values.Select(u => JsonValue.Of(("id", JsonValue.Of(u.Id.Value)), ("state", JsonValue.Of(EnumWire.Name(u.State)))))),
                        "db units");
                if (wantDb["intents"] is { } intents)
                    Same(intents, JsonValue.Of(db.OpenIntents().Result.Select(i => JsonValue.Of(i.UnitId.Value))), "db intents");
            }
            return true;
        });
    }
}
