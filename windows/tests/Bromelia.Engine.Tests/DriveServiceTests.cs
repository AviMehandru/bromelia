using System.Text;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Xunit;
using static Bromelia.Tests.Fixtures;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Engine.Tests;

/// <summary>shared/fixtures/engine/drive-service.cases.json, on a real executor, bus, problem service and engine clock
/// with fake ports.</summary>
public sealed class DriveServiceTests
{
    static readonly TimeSpan Patience = TimeSpan.FromSeconds(10);
    static readonly List<KeyValuePair<string, JsonValue>> None = new();

    static BroFailure Failure(JsonValue j) =>
        new(new BroError(j["code"]!.AsString!, j["params"] as JsonValue.Object ?? new JsonValue.Object(new List<KeyValuePair<string, JsonValue>>())));

    static JsonValue ErrorJson(BroError e) => JsonValue.Of(("error", JsonValue.Of(("code", JsonValue.Of(e.Code)), ("params", e.Params))));

    sealed class Fakes : IMakemkv, IDeviceMonitor, IDriveControl, IDriveRepository, IConfigSource
    {
        readonly object _lock = new();
        public readonly List<string> Calls = new();
        public List<JsonValue> Makemkv = new();
        public List<OsDrive> Os = new();
        public JsonValue? ScanFails, EjectFails;
        public Action<DeviceEvent>? Sink;
        public readonly SortedDictionary<string, DriveRecord> Stored = new(StringComparer.Ordinal);
        public Config Config = ConfigCodec.Decode(Encoding.UTF8.GetBytes("{\"version\": 3}"));

        void Call(string c)
        {
            lock (_lock) Calls.Add(c);
        }

        public Task<IReadOnlyList<MakemkvDrive>> ScanDrives(CancellationToken cancel)
        {
            Call("scan");
            JsonValue? fails;
            List<JsonValue> lines;
            lock (_lock) (fails, lines) = (ScanFails, Makemkv.ToList());
            if (fails != null) throw Failure(fails);
            return Task.FromResult<IReadOnlyList<MakemkvDrive>>(lines.Select(l => MakemkvDrive.From(Robot.ParseLine(l.AsString!)!)!).ToList());
        }

        public Task<ListingRun> Listing(MakemkvSource source, MakemkvInvocation invocation, IRunSink sink, CancellationToken cancel) => throw new NotSupportedException();
        public Task<MakemkvRun> Rip(MakemkvSource source, string title, string destination, MakemkvInvocation invocation, IRunSink sink, CancellationToken cancel) =>
            throw new NotSupportedException();
        public Task<MakemkvRun> Backup(MakemkvSource source, bool decrypt, string destination, MakemkvInvocation invocation, IRunSink sink, CancellationToken cancel) =>
            throw new NotSupportedException();

        public void Start(Action<DeviceEvent> sink)
        {
            Call("monitor.start");
            lock (_lock) Sink = sink;
        }

        public void Stop() { }

        public IReadOnlyList<OsDrive> CurrentDrives()
        {
            lock (_lock) return Os.ToList();
        }

        public Task Eject(string device)
        {
            Call("eject " + device);
            return EjectFails is { } f ? throw Failure(f) : Task.CompletedTask;
        }

        public Task CloseTray(string device)
        {
            Call("closeTray " + device);
            return Task.CompletedTask;
        }

        public Task<string?> WaitForMount(string device, Duration timeout, CancellationToken cancel) => throw new NotSupportedException();

        public DiscContent ProbeContent(string device)
        {
            Call("probe " + device);
            return DiscContent.Video;
        }

        public ISectorReader OpenRaw(string device) => throw new NotSupportedException();

        public Task Upsert(DriveRecord drive)
        {
            lock (_lock) Stored[drive.Id] = drive;
            return Task.CompletedTask;
        }

        public Task<IReadOnlyList<DriveRecord>> All()
        {
            lock (_lock) return Task.FromResult<IReadOnlyList<DriveRecord>>(Stored.Values.ToList());
        }

        public Task RecordStats(string driveId, string day, DriveStats stats) => throw new NotSupportedException();

        public Config Current() => Config;
    }

    static OsDrive OsOf(JsonValue o) => new(o["device"]!.AsString!, o["identification"]?.AsString ?? "", o["mountPath"]?.AsString);

    static DriveRecord RowOf(JsonValue r) => new(r["id"]!.AsString!, r["identification"]!.AsString!, r["model"]!.AsString!, r["lastDevice"]!.AsString!,
        r["configId"]?.AsString, Instant.Parse(r["firstSeenAt"]!.AsString!)!.Value, Instant.Parse(r["lastSeenAt"]!.AsString!)!.Value);

    static JsonValue RowJson(DriveRecord r) => JsonValue.Of(("id", JsonValue.Of(r.Id)), ("identification", JsonValue.Of(r.Identification)),
        ("model", JsonValue.Of(r.Model)), ("lastDevice", JsonValue.Of(r.LastDevice)), ("configId", JsonValue.Of(r.ConfigId)),
        ("firstSeenAt", JsonValue.Of(Instant.Format(r.FirstSeenAt))), ("lastSeenAt", JsonValue.Of(Instant.Format(r.LastSeenAt))));

    static JsonValue LocationJson(DriveLocation? l) => l == null ? JsonValue.Null.Instance : JsonValue.Of(
        ("makemkvIndex", l.MakemkvIndex is { } i ? JsonValue.Of(i) : JsonValue.Null.Instance), ("device", JsonValue.Of(l.Device)),
        ("configId", JsonValue.Of(l.ConfigId)), ("generation", l.Generation is { } g ? JsonValue.Of(g) : JsonValue.Null.Instance),
        ("label", JsonValue.Of(l.Label)), ("flags", l.Flags is { } f ? JsonValue.Of(f.Raw) : JsonValue.Null.Instance));

    static DeviceEvent EventOf(JsonValue e)
    {
        var device = e["device"]!.AsString!;
        return e["type"]!.AsString switch
        {
            "driveAppeared" => new DeviceEvent.DriveAppeared(new OsDrive(device)),
            "driveVanished" => new DeviceEvent.DriveVanished(device),
            "mediaArrived" => new DeviceEvent.MediaArrived(device),
            "mediaRemoved" => new DeviceEvent.MediaRemoved(device),
            "trayOpened" => new DeviceEvent.TrayOpened(device),
            "mounted" => new DeviceEvent.Mounted(device, e["path"]!.AsString!),
            "unmounted" => new DeviceEvent.Unmounted(device),
            var t => throw new InvalidOperationException("unknown device event " + t),
        };
    }

    static void Same(JsonValue want, JsonValue got, string what)
    {
        var w = Encoding.UTF8.GetString(JsonValue.EncodeCanonical(want));
        var g = Encoding.UTF8.GetString(JsonValue.EncodeCanonical(got));
        if (w != g) throw new InvalidOperationException(what + ": expected\n" + w + "got\n" + g);
    }

    [Fact]
    public void TheSharedCasesPass()
    {
        RunCases("engine/drive-service.cases.json", (id, given, expect) =>
        {
            using var w = new EngineClockTests.World();
            var bus = new EventBus(w.Clock);
            var problems = new ProblemService(bus, w.Clock);
            var f = new Fakes
            {
                Makemkv = given["makemkv"]?.AsArray?.ToList() ?? new(),
                Os = (given["os"]?.AsArray ?? new List<JsonValue>()).Select(OsOf).ToList(),
                EjectFails = given["ejectFails"],
            };
            if (given["config"] is { } doc) f.Config = ConfigCodec.Decode(JsonValue.EncodeCanonical(doc));
            foreach (var r in given["stored"]?.AsArray ?? new List<JsonValue>()) f.Stored[r["id"]!.AsString!] = RowOf(r);
            var service = new DriveService(w.Executor, bus, problems, f, w.EngineClock, f, f, f, f);
            var events = new List<JsonValue>();
            w.OnExecutor(() => bus.Subscribe(new[] { EngineEventType.DriveChanged, EngineEventType.DriveRemoved, EngineEventType.MediumArrived,
                EngineEventType.MediumRemoved, EngineEventType.ProblemRaised, EngineEventType.ProblemCleared },
                e => events.Add(JsonValue.Of(("seq", JsonValue.Of(e.Seq)), ("at", JsonValue.Of(Instant.Format(e.At))),
                    ("type", JsonValue.Of(EnumWire.Name(e.Type))), ("data", e.Data)))));

            bool Settled()
            {
                var ok = false;
                var checkedIt = new ManualResetEventSlim();
                w.Executor.Post(() =>
                {
                    ok = w.Executor.Activity == 0;
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

            // Runs a command on the executor and waits for it; its failure is the result.
            JsonValue? Run(Func<Task> command)
            {
                var done = new ManualResetEventSlim();
                JsonValue? result = null;
                w.Executor.Post(async () =>
                {
                    try { await command(); }
                    catch (BroFailure failure) { result = ErrorJson(failure.Error); }
                    done.Set();
                });
                if (!done.Wait(Patience)) throw new TimeoutException("a command didn't finish");
                Settle();
                return result;
            }

            var results = new List<JsonValue>();
            foreach (var op in given["ops"]!.AsArray!)
            {
                var name = op.AsString ?? op.AsObject![0].Key;
                var arg = op.AsString != null ? JsonValue.Null.Instance : op.AsObject![0].Value;
                JsonValue? result = null;
                switch (name)
                {
                    case "start": result = Run(service.Start); break;
                    case "set":
                        if (arg["makemkv"]?.AsArray is { } mk) f.Makemkv = mk.ToList();
                        if (arg["os"]?.AsArray is { } os) f.Os = os.Select(OsOf).ToList();
                        if (arg.AsObject!.Any(m => m.Key == "scanFails")) f.ScanFails = arg["scanFails"] is { IsNull: false } sf ? sf : null;
                        break;
                    case "device":
                        f.Sink!(EventOf(arg));
                        Settle();
                        break;
                    case "advance": w.Clock.Advance(arg.AsInteger!.Value * 1000); Settle(); break;
                    case "lease":
                        var json = JsonValue.Of(("drives", arg["drives"]!), ("acquisitions", arg["acquisitions"]!),
                            ("jobs", JsonValue.Of(("acquisition", JsonValue.Of(0)), ("processing", JsonValue.Of(0)), ("maintenance", JsonValue.Of(0)))));
                        w.OnExecutor(() => bus.Publish(EngineEventType.LeaseChanged, json));
                        Settle();
                        break;
                    case "config":
                        w.OnExecutor(() =>
                        {
                            f.Config = ConfigCodec.Decode(JsonValue.EncodeCanonical(arg));
                            bus.Publish(EngineEventType.ConfigChanged, JsonValue.Of(("version", JsonValue.Of(2))));
                        });
                        Settle();
                        break;
                    case "rescan": result = Run(service.Rescan); break;
                    case "eject": result = Run(() => service.Eject(arg.AsString!)); break;
                    case "closeTray": result = Run(() => service.CloseTray(arg.AsString!)); break;
                    case "closeAll": result = Run(service.CloseAll); break;
                    case "drives":
                        IReadOnlyList<DriveDto> drives = Array.Empty<DriveDto>();
                        w.OnExecutor(() => drives = service.Drives());
                        result = JsonValue.Of(drives.Select(DriveDto.Json));
                        break;
                    case "find": result = LocationJson(service.Find(arg.AsString!)); break;
                    default: throw new InvalidOperationException("unknown op " + name);
                }
                results.Add(result ?? JsonValue.Null.Instance);
            }
            w.OnExecutor(service.Stop);
            Settle();

            if (expect["results"] is { } wantResults) Same(wantResults, JsonValue.Of(results), "results");
            if (expect["events"] is { } wantEvents) Same(wantEvents, JsonValue.Of(events), "events");
            if (expect["calls"] is { } wantCalls) Same(wantCalls, JsonValue.Of(f.Calls.Select(c => JsonValue.Of(c))), "calls");
            if (expect["problems"] is { } wantProblems)
            {
                IReadOnlyList<ProblemDto> list = Array.Empty<ProblemDto>();
                w.OnExecutor(() => list = problems.Problems());
                Same(wantProblems, JsonValue.Of(list.Select(ProblemDto.Json)), "problems");
            }
            if (expect["stored"] is { } wantStored) Same(wantStored, JsonValue.Of(f.Stored.Values.Select(RowJson)), "stored");
            return true;
        });
    }
}
