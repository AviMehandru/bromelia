using Bromelia.Domain;
using Bromelia.Foundation;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Domain.Tests;

public class RulesTests
{
    private static List<string>? Strings(JsonValue? v) => v?.AsArray?.Select(x => x.AsString!).ToList();

    private static RuleFacts Facts(JsonValue f) => new(f["name"]?.AsString ?? "", f["label"]?.AsString ?? "", f["formatCode"]?.AsString ?? "",
        f["kind"]?.AsString, f["driveId"]?.AsString, f["profileId"]?.AsString, f["automatic"]?.AsBool ?? false);

    private static RuleWhen When(JsonValue w) => new(w["nameOrLabel"]?.AsString, w["name"]?.AsString, w["label"]?.AsString,
        Strings(w["formats"]), Strings(w["kinds"]), Strings(w["drives"]), Strings(w["profiles"]), w["automatic"]?.AsBool);

    /// <summary>Whether every key of <paramref name="expected"/> is in <paramref name="actual"/> with the same
    /// value (objects compared the same way, anything else exactly).</summary>
    private static void Subset(JsonValue expected, JsonValue? actual, string path)
    {
        if (expected is JsonValue.Object o)
        {
            foreach (var m in o.Members) Subset(m.Value, actual?[m.Key], path + "." + m.Key);
            return;
        }
        Same(expected, actual, path);
    }

    [Fact]
    public void RuleCases() => RunCases("domain/rules.cases.json", id => !id.StartsWith("drive-") && id != "invalid-regex-issue", (id, given, expect) =>
    {
        if (given["when"] is { } when)
        {
            Same(expect["matches"]!.AsBool, RuleMatcher.Matches(When(when), Facts(given["facts"]!)), "matches");
            return true;
        }
        if (id.StartsWith("mode-"))
        {
            int raw = 0;
            foreach (var f in Strings(given["flags"]) ?? new List<string>())
                raw |= f switch { "dvdFiles" => 1, "hdDvdFiles" => 2, "blurayFiles" => 4, _ => throw new Xunit.Sdk.XunitException(f) };
            var mode = ModeChooser.Mode(EnumWire.Parse<DiscFormat>(given["format"]?.AsString), new DiscFlags(raw),
                EnumWire.Parse<DiscContent>(given["content"]?.AsString) ?? DiscContent.Unknown, new Profile(given["profile"]!), false);
            Same(expect["mode"], mode is { } m ? JsonValue.Of(EnumWire.Name(m)) : JsonValue.Null.Instance, "mode");
            return true;
        }
        if (given["config"] is { } config)
        {
            var effective = ProfileResolver.Resolve(ConfigTests.Decode(config), given["drive"]?.AsString, Facts(given["facts"]!), given["session"]);
            Subset(expect["profile"]!, effective.Profile, "profile");
            Same(expect["steps"], JsonValue.Of(effective.Steps.Select(JsonValue.Of)), "steps");
            Same(expect["trace"], JsonValue.Of(effective.Trace.Select(t => t.Id is { } tid
                ? JsonValue.Of(("layer", JsonValue.Of(EnumWire.Name(t.Layer))), ("id", JsonValue.Of(tid)))
                : JsonValue.Of(("layer", JsonValue.Of(EnumWire.Name(t.Layer)))))), "trace");
            return true;
        }
        return false;
    });

    [Fact]
    public void StepFilterCases() => RunCases("domain/arguments.cases.json", id => id.StartsWith("run-condition") || id == "status-words", (id, given, expect) =>
    {
        if (given["outcomes"] is { } outcomes)
        {
            Same(expect["statusWords"], JsonValue.Of(outcomes.AsArray!.Select(o =>
                JsonValue.Of(EnumWire.Name(StepFilter.StatusWord(EnumWire.Parse<Outcome>(o.AsString)!.Value))))), "statusWords");
            return true;
        }
        var step = StepDefinition.Decode(JsonValue.Of(new[] { ("id", JsonValue.Of("s")), ("kind", JsonValue.Of("command")) }
            .Concat(given["step"]!.AsObject!.Select(m => (m.Key, m.Value))).ToArray()));
        Same(expect["runs"]!.AsBool, StepFilter.Applies(step, EnumWire.Parse<Outcome>(given["outcome"]!.AsString)!.Value), "runs");
        return true;
    });

    [Fact]
    public void RuleProfileSwitchAndRemovedKeys()
    {
        var config = ConfigTests.Decode(JsonValue.Parse("""
            {"version": 3,
             "profiles": [{"id": "default", "titles": {"strategy": "longest"}}, {"id": "anime", "titles": {"strategy": "all"}}],
             "rules": [{"id": "a", "when": {"nameOrLabel": "piece"}, "then": {"profile": "anime"}},
                       {"id": "b", "when": {"profiles": ["anime"]}, "then": {"set": {"titles": {"strategy": null}}}},
                       {"id": "off", "enabled": false, "when": {}, "then": {"steps": ["x"]}}]}
            """)!);
        var effective = ProfileResolver.Resolve(config, null, new RuleFacts("One Piece", "", "DVD"), null);
        Assert.Equal(new[] { ResolveLayer.Defaults, ResolveLayer.DefaultProfile, ResolveLayer.DriveProfile, ResolveLayer.Rule, ResolveLayer.Rule },
            effective.Trace.Select(t => t.Layer).ToArray());
        // Rule b removed the strategy, so the default comes back rather than the default profile's.
        var defaults = ProfileResolver.Resolve(ConfigTests.Decode(JsonValue.Parse("""{"version": 3}""")!), null, new RuleFacts("", "", ""), null).Profile;
        Same(defaults["titles"]!["strategy"], effective.Profile["titles"]!["strategy"], "strategy");
        Assert.Equal(new[] { "titles.strategy" }, effective.Trace[4].Keys);
        Assert.Empty(effective.Steps);
    }
}
