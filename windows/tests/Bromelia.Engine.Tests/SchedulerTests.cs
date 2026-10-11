using Bromelia.Domain;
using Bromelia.Foundation;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Engine.Tests;

/// <summary>shared/fixtures/engine/scheduler.cases.json, on a real executor.</summary>
public sealed class SchedulerTests
{
    static SchedulerLimits Limits(JsonValue? j) => new(
        (int)(j?["acquisitions"]?.AsInteger ?? 0), (int)(j?["processing"]?.AsInteger ?? 1),
        (int)(j?["cpu"]?.AsInteger ?? 2), (int)(j?["ioPerVolume"]?.AsInteger ?? 2));

    static List<string> Strings(JsonValue? j) => j?.AsArray?.Select(s => s.AsString!).ToList() ?? new List<string>();

    sealed class Harness
    {
        public readonly EngineClockTests.World W = new();
        public readonly EventBus Bus;
        public readonly Scheduler Scheduler;
        public readonly Dictionary<string, Id> Jobs = new();
        public readonly List<string> Granted = new();
        public readonly Dictionary<string, string> States = new();
        public readonly Dictionary<string, List<string>> Waits = new();
        public readonly Dictionary<string, Lease> Leases = new();
        public readonly Dictionary<string, CancellationSource> Cancels = new();
        public readonly List<EngineEvent> Events = new();

        public Harness(JsonValue given)
        {
            Bus = new EventBus(W.Clock);
            Scheduler = new Scheduler(W.Executor, Bus, Limits(given["limits"]));
            W.OnExecutor(() => Bus.Subscribe(null, Events.Add));
        }

        public Id Job(string name)
        {
            if (!Jobs.TryGetValue(name, out var id))
                Jobs[name] = id = new Id($"00000000-0000-4000-8000-{Jobs.Count + 1:x12}");
            return id;
        }

        string NameOf(Id id) => Jobs.First(j => j.Value == id).Key;

        async void Track(string name, Task<Lease> task)
        {
            try
            {
                Leases[name] = await task;
                Granted.Add(name);
                States[name] = "granted";
            }
            catch (BroFailure f)
            {
                States[name] = f.Error.Code == "job.cancelled" ? "cancelled" : "failed " + f.Error.Code;
            }
        }

        static void Expecting(JsonValue? expectError, Action action)
        {
            try
            {
                action();
                if (expectError != null) throw new InvalidOperationException("expected the error " + expectError);
            }
            catch (BroFailure f) when (expectError != null)
            {
                if (expectError.AsString is { } code) Same(code, f.Error.Code, "error");
                else
                {
                    Same(expectError["code"]!.AsString, f.Error.Code, "error");
                    Same<JsonValue>(expectError["params"]!, f.Error.Params, "error params");
                }
            }
        }

        public void Run(JsonValue op)
        {
            var (name, arg) = op.AsObject!.Single();
            switch (name)
            {
                case "submit":
                    Scheduler.Submit(Job(arg["job"]!.AsString!), EnumWire.Parse<Queue>(arg["queue"]!.AsString)!.Value);
                    break;
                case "request":
                {
                    var @as = arg["as"]!.AsString!;
                    var res = new ResourceSet(Strings(arg["drives"]), arg["acquisition"]?.AsBool == true, Strings(arg["libraryWrites"]),
                        (int)(arg["cpu"]?.AsInteger ?? 0), Strings(arg["io"]));
                    var cancel = Cancels[@as] = new CancellationSource();
                    Waits[@as] = new List<string>();
                    Track(@as, Scheduler.Acquire(Job(arg["job"]!.AsString!), res, wait =>
                    {
                        var text = wait is Wait.Blocked b ? "blocked:" + b.Reason.Code : "waiting";
                        Waits[@as].Add(text);
                        States[@as] = text;
                    }, cancel.Token));
                    break;
                }
                case "release":
                    Scheduler.Release(Leases[arg.AsString!]);
                    States[arg.AsString!] = "released";
                    break;
                case "releaseDrives":
                    Scheduler.ReleaseDrives(Job(arg.AsString!));
                    break;
                case "toProcessing":
                    Scheduler.MoveTo(Job(arg.AsString!), Queue.Processing);
                    break;
                case "finish":
                {
                    var job = Job(arg.AsString!);
                    foreach (var (n, lease) in Leases)
                        if (lease.JobId == job && States[n] == "granted") States[n] = "released";
                    Scheduler.Finish(job);
                    break;
                }
                case "move":
                    Expecting(arg["expectError"], () => Scheduler.Move(Job(arg["job"]!.AsString!), (int)arg["by"]!.AsInteger!));
                    break;
                case "startNow":
                    Expecting(arg["expectError"], () => Scheduler.StartNow(Job(arg["job"]!.AsString!)));
                    break;
                case "cancel":
                    Cancels[arg.AsString!].Cancel();
                    break;
                case "cancelAll":
                    Scheduler.CancelAll(EnumWire.Parse<Queue>(arg.AsString)!.Value);
                    break;
                case "library":
                {
                    var id = arg["id"]!.AsString!;
                    Scheduler.SetLibraryOffline(id, arg["online"]!.AsBool == true ? null
                        : new BroError(MessageCode.Wire(MessageCode.LibraryOffline), ("library", JsonValue.Of(id))));
                    break;
                }
                case "limits":
                    Scheduler.UpdateLimits(Limits(arg));
                    break;
                case "reserve":
                    Expecting(arg["expectError"], () => Scheduler.Reserve(Job(arg["job"]!.AsString!), arg["volume"]!.AsString!,
                        arg["path"]!.AsString!, arg["need"]!.AsInteger!.Value, arg["free"]!.AsInteger!.Value));
                    break;
                case "unreserve":
                    Scheduler.Unreserve(Job(arg.AsString!));
                    break;
                default:
                    throw new InvalidOperationException("unknown op " + name);
            }
        }

        public JsonValue QueuesJson()
        {
            var q = Scheduler.Queues();
            JsonValue Names(IReadOnlyList<Id> ids) => JsonValue.Of(ids.Select(id => JsonValue.Of(NameOf(id))));
            return JsonValue.Of(("acquisition", Names(q.Acquisition)), ("processing", Names(q.Processing)), ("maintenance", Names(q.Maintenance)));
        }
    }

    [Fact]
    public void TheSharedCasesPass()
    {
        RunCases("engine/scheduler.cases.json", (id, given, expect) =>
        {
            var h = new Harness(given);
            try
            {
                foreach (var op in given["ops"]!.AsArray!)
                {
                    Exception? failure = null;
                    h.W.OnExecutor(() =>
                    {
                        try { h.Run(op); }
                        catch (Exception e) { failure = e; }
                    });
                    h.W.Settle();
                    if (failure != null) throw failure;
                }
                JsonValue? got = null;
                h.W.OnExecutor(() => got = JsonValue.Of(
                    ("granted", JsonValue.Of(h.Granted.Select(g => JsonValue.Of(g)))),
                    ("queues", h.QueuesJson()),
                    ("leases", LeaseSnapshot.Json(h.Scheduler.Leases())),
                    ("events", JsonValue.Of(h.Events.Select(EngineEvent.Json)))));
                if (expect["granted"] is { } granted) Same(granted, got!["granted"]!, "granted");
                foreach (var (name, state) in expect["states"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
                    Same(state.AsString, h.States.GetValueOrDefault(name), "state of " + name);
                foreach (var (name, waits) in expect["waits"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
                    Same(string.Join(", ", Strings(waits)), string.Join(", ", h.Waits.GetValueOrDefault(name) ?? new List<string>()), "waits of " + name);
                if (expect["queues"] is { } queues) Same(queues, got!["queues"]!, "queues");
                if (expect["leases"] is { } leases) Same(leases, got!["leases"]!, "leases");
                if (expect["events"] is { } events) Same(events, got!["events"]!, "events");
                return true;
            }
            finally
            {
                h.W.Dispose();
            }
        });
    }

    [Fact]
    public void ALeaseSnapshotReadsBackAsWritten()
    {
        var snapshot = new LeaseSnapshot(new[] { new DriveHolder("drv-0123456789abcdef", new Id("00000000-0000-4000-8000-000000000001")) }, 1,
            new JobCounts(1, 2, 3));
        Assert.Equal(snapshot, LeaseSnapshot.Parse(LeaseSnapshot.Json(snapshot)));
        Assert.Null(LeaseSnapshot.Parse(JsonValue.Of(("drives", JsonValue.Of(1L)))));
    }
}
