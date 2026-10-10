using Bromelia.Foundation;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Engine.Tests;

/// <summary>shared/fixtures/engine/event-bus.cases.json.</summary>
public sealed class EventBusTests
{
    static string Line(EngineEvent e) => $"{e.Seq} {EnumWire.Name(e.Type)}";

    static List<EngineEventType>? Types(JsonValue? j) =>
        j?.AsArray?.Select(t => EnumWire.Parse<EngineEventType>(t.AsString) ?? throw new InvalidOperationException("unknown type " + t)).ToList();

    sealed class Harness
    {
        public readonly ManualClock Clock = new();
        public readonly EventBus Bus;
        public readonly Dictionary<string, Subscription> Subs = new();
        public readonly Dictionary<string, List<string>> Received = new();
        public readonly Dictionary<string, EventStream> Streams = new();
        public readonly Dictionary<string, List<string>> Read = new();
        public readonly List<EngineEvent> Published = new();

        public Harness(JsonValue given)
        {
            Bus = new EventBus(Clock, given["firstSeq"]?.AsInteger ?? 1, (int)(given["retain"]?.AsInteger ?? 1024),
                (int)(given["streamCapacity"]?.AsInteger ?? 1024));
        }

        public void Run(IEnumerable<JsonValue> ops)
        {
            foreach (var op in ops)
            {
                var (name, arg) = op.AsObject!.Single();
                switch (name)
                {
                    case "publish":
                        Published.Add(Bus.Publish(EnumWire.Parse<EngineEventType>(arg["type"]!.AsString)!.Value,
                            arg["data"] ?? JsonValue.Of(Array.Empty<(string, JsonValue)>())));
                        break;
                    case "advance":
                        Clock.Advance(arg.AsInteger!.Value);
                        break;
                    case "subscribe":
                    {
                        var @as = arg["as"]!.AsString!;
                        var got = Received[@as] = new List<string>();
                        var on = arg["onReceive"];
                        Subs[@as] = Bus.Subscribe(Types(arg["types"]), e =>
                        {
                            got.Add(Line(e));
                            if (on != null && on["type"]!.AsString == EnumWire.Name(e.Type)) Run(on["do"]!.AsArray!);
                        });
                        break;
                    }
                    case "unsubscribe":
                        Subs[arg.AsString!].Cancel();
                        break;
                    case "stream":
                    {
                        var @as = arg["as"]!.AsString!;
                        Streams[@as] = Bus.Stream(arg["after"]?.AsInteger, Types(arg["types"]));
                        Read[@as] = new List<string>();
                        break;
                    }
                    case "read":
                    {
                        var s = Streams[arg.AsString!];
                        var log = Read[arg.AsString!];
                        while (s.Poll() is { } item)
                            log.Add(item is StreamItem.Event ev ? Line(ev.Value) : "resync");
                        if (s.IsClosed) log.Add("end");
                        break;
                    }
                    case "close":
                        Streams[arg.AsString!].Close();
                        break;
                    default:
                        throw new InvalidOperationException("unknown op " + name);
                }
            }
        }
    }

    static void SameLines(JsonValue want, List<string> got, string what) =>
        Same(string.Join(" | ", want.AsArray!.Select(w => w.AsString)), string.Join(" | ", got), what);

    [Fact]
    public void TheSharedCasesPass()
    {
        RunCases("engine/event-bus.cases.json", (id, given, expect) =>
        {
            if (id == "event-types")
            {
                var all = Enum.GetValues<EngineEventType>();
                SameLines(expect["clientVisible"]!, all.Where(t => !t.IsInternal()).Select(EnumWire.Name).ToList(), "client-visible types");
                SameLines(expect["internal"]!, all.Where(t => t.IsInternal()).Select(EnumWire.Name).ToList(), "internal types");
                var schema = SharedJson(expect["asInSchema"]!.AsString!);
                var consts = schema["oneOf"]!.AsArray!.Select(r => r["$ref"]!.AsString!.Split('/').Last())
                    .Select(d => schema["$defs"]![d]!["properties"]!["type"]!["const"]!).ToList();
                SameLines(new JsonValue.Array(consts), all.Where(t => !t.IsInternal()).Select(EnumWire.Name).ToList(), "events.json");
                return true;
            }
            var h = new Harness(given);
            h.Run(given["ops"]!.AsArray!);
            if (expect["lastSeq"] is { } last) Same(last.AsInteger, (long?)h.Bus.LastSeq, "lastSeq");
            if (expect["events"] is { } events)
                Same(events, (JsonValue)new JsonValue.Array(h.Published.Select(EngineEvent.Json).ToList()), "events");
            foreach (var (name, want) in expect["received"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
                SameLines(want, h.Received[name], "received by " + name);
            foreach (var (name, want) in expect["read"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
                SameLines(want, h.Read[name], "read from " + name);
            return true;
        });
    }

    [Fact]
    public async Task NextWaitsForAnEventAndEndsWhenClosed()
    {
        var bus = new EventBus(new ManualClock());
        var stream = bus.Stream(null, null);
        var next = stream.Next();
        Assert.False(next.IsCompleted);
        bus.Publish(EngineEventType.JobChanged, JsonValue.Of(Array.Empty<(string, JsonValue)>()));
        var item = Assert.IsType<StreamItem.Event>(await next.WaitAsync(TimeSpan.FromSeconds(5)));
        Assert.Equal(1, item.Value.Seq);
        var end = stream.Next();
        Assert.False(end.IsCompleted);
        stream.Close();
        Assert.Null(await end.WaitAsync(TimeSpan.FromSeconds(5)));
    }
}
