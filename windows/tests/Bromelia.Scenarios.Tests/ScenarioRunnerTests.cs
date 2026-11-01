using Bromelia.Foundation;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Scenarios.Tests;

/// <summary>Every shared scenario this runner selects (ScenarioCatalog), each its own test case named by its id; a selected
/// scenario that fails is a failure, never a skip. shared/fixtures/engine/scenario-runner.cases.json checks the runner
/// itself: each broken variant of a scenario must fail, with the failure text the case gives.</summary>
public sealed class ScenarioRunnerTests
{
    public static IEnumerable<object[]> Selected() => ScenarioCatalog.Selected().Select(id => new object[] { id });

    [Theory]
    [MemberData(nameof(Selected))]
    public Task Scenario(string id) => Task.Run(() => ScenarioRun.Play(id));

    [Fact]
    public void TheRunnerCatchesBrokenScenarios() => RunCases("engine/scenario-runner.cases.json", (id, given, expect) =>
    {
        var name = given["scenario"]!.AsString!;
        var broken = SetAt(ScenarioCatalog.Load(name), given["pointer"]!.AsString!, given["value"]!);
        var failure = Failure(name, broken);
        var wanted = expect["failure"]!.AsString!;
        if (failure == null) throw new Xunit.Sdk.XunitException($"the broken {name} passed; wanted a failure with: {wanted}");
        if (!failure.Contains(wanted, StringComparison.Ordinal))
            throw new Xunit.Sdk.XunitException($"the broken {name} failed with:\n{failure}\nwanted it to contain:\n{wanted}");
        return true;
    });

    /// <summary>The failure text of playing the scenario; null when it passes.</summary>
    static string? Failure(string id, JsonValue scenario)
    {
        try
        {
            Task.Run(() => ScenarioRun.Play(id, scenario)).GetAwaiter().GetResult();
            return null;
        }
        catch (ScenarioFailure f)
        {
            return f.Message;
        }
    }

    /// <summary>The document with the value at the JSON pointer (RFC 6901) replaced, or added to its object.</summary>
    static JsonValue SetAt(JsonValue doc, string pointer, JsonValue value)
    {
        var parts = pointer.Split('/').Skip(1).Select(p => p.Replace("~1", "/").Replace("~0", "~")).ToList();
        JsonValue Set(JsonValue at, int i)
        {
            if (i == parts.Count) return value;
            var key = parts[i];
            if (at is JsonValue.Array a)
            {
                var items = a.Items.ToList();
                int n = int.Parse(key);
                items[n] = Set(items[n], i + 1);
                return new JsonValue.Array(items);
            }
            var members = (at as JsonValue.Object)?.Members.ToList() ?? new List<KeyValuePair<string, JsonValue>>();
            int k = members.FindIndex(m => m.Key == key);
            var next = Set(k >= 0 ? members[k].Value : JsonValue.Of(), i + 1);
            if (k >= 0) members[k] = new(key, next);
            else members.Add(new(key, next));
            return new JsonValue.Object(members);
        }
        return Set(doc, 0);
    }
}
