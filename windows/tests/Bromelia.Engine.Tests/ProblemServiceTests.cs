using Bromelia.Domain;
using Bromelia.Foundation;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Engine.Tests;

/// <summary>shared/fixtures/engine/problem-service.cases.json, on a real executor.</summary>
public sealed class ProblemServiceTests
{
    internal static ProblemScope? Scope(JsonValue? j) =>
        j == null ? null : new ProblemScope(EnumWire.Parse<ProblemScopeKind>(j["kind"]!.AsString)!.Value, j["id"]!.AsString!);

    [Fact]
    public void TheSharedCasesPass()
    {
        RunCases("engine/problem-service.cases.json", (id, given, expect) =>
        {
            using var w = new EngineClockTests.World();
            var bus = new EventBus(w.Clock);
            var problems = new ProblemService(bus, w.Clock);
            var events = new List<EngineEvent>();
            w.OnExecutor(() => bus.Subscribe(null, events.Add));
            foreach (var op in given["ops"]!.AsArray!)
            {
                var (name, arg) = op.AsObject!.Single();
                switch (name)
                {
                    case "raise":
                        w.OnExecutor(() => problems.Raise(
                            new BroMessage(MessageCode.Parse(arg["code"]!.AsString!)!.Value,
                                arg["params"] as JsonValue.Object ?? new JsonValue.Object(new List<KeyValuePair<string, JsonValue>>()),
                                EnumWire.Parse<Severity>(arg["severity"]!.AsString)!.Value),
                            Scope(arg["scope"]),
                            arg["actions"]?.AsArray?.Select(a => EnumWire.Parse<ProblemAction>(a.AsString)!.Value).ToList()));
                        break;
                    case "clear":
                        w.OnExecutor(() => problems.Clear(MessageCode.Parse(arg["code"]!.AsString!)!.Value, Scope(arg["scope"])));
                        break;
                    case "advance":
                        w.Clock.Advance(arg.AsInteger!.Value * 1000);
                        break;
                    default:
                        throw new InvalidOperationException("unknown op " + name);
                }
            }
            JsonValue? got = null;
            w.OnExecutor(() => got = JsonValue.Of(
                ("problems", JsonValue.Of(problems.Problems().Select(ProblemDto.Json))),
                ("events", JsonValue.Of(events.Select(EngineEvent.Json)))));
            Same(expect["problems"]!, got!["problems"]!, "problems");
            Same(expect["events"]!, got!["events"]!, "events");
            return true;
        });
    }
}
