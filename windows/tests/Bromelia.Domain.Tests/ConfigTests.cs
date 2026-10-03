using System.Text;
using Bromelia.Domain;
using Bromelia.Foundation;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Domain.Tests;

public class ConfigTests
{
    internal static Config Decode(JsonValue json) => ConfigCodec.Decode(JsonValue.EncodeCanonical(json));

    internal static string Encoded(Config c) => Encoding.UTF8.GetString(ConfigCodec.Encode(c));

    /// <summary>Issues as the fixtures write them: params only when the expected issue has them.</summary>
    internal static void SameIssues(JsonValue expected, IReadOnlyList<Issue> actual)
    {
        var want = expected.AsArray!;
        var got = actual.Select((x, i) =>
        {
            var j = x.ToJson();
            bool withParams = i < want.Count && want[i]["params"] != null;
            return withParams ? j : JsonValue.Of(j.AsObject!.Where(m => m.Key != "params").Select(m => (m.Key, m.Value)).ToArray());
        }).ToList();
        Same(expected, JsonValue.Of(got), "issues");
    }

    [Fact]
    public void ConfigCodecCases() => RunCases("config/config-codec.cases.json", (id, given, expect) =>
    {
        if (given["template"] is { } template)
        {
            Assert.Equal("archiveEverything", template.AsString);
            Same(Sorted(expect["profile"]!), Sorted(ConfigTemplates.ArchiveEverything().Json), "profile");
            return true;
        }
        if (given["catalog"] is { } catalog)
        {
            // The settings catalog the forms are generated from (no Domain function reads it: it's the apps').
            var doc = Json("../" + catalog.AsString!.Substring("shared/".Length));
            Assert.Contains("\"" + expect["hasKey"]!.AsString! + "\"", doc.ToString());
            Assert.True(doc["selectionPresets"]?.AsArray?.Count > 0);
            return true;
        }
        var config = given["file"] is { } file ? ConfigCodec.Decode(File.ReadAllBytes(Path_("config/" + file.AsString!))) : Decode(given["json"]!);
        var encoded = Encoded(config);
        if (expect["equals"] is { } eq) Same(Text("config/" + eq.AsString!), encoded, "encoded");
        // Decoding what was encoded changes nothing (the file itself is formatted by hand).
        if (expect["roundTrips"]?.AsBool == true) Same(encoded, Encoded(ConfigCodec.Decode(ConfigCodec.Encode(config))), "round trip");
        foreach (var m in expect["paths"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
            Same(m.Value, At(config.Document, m.Key), m.Key);
        foreach (var c in expect["contains"]?.AsArray ?? new List<JsonValue>())
            if (!encoded.Contains(c.AsString!)) throw new Xunit.Sdk.XunitException($"encoded doesn't contain {c.AsString}");
        foreach (var c in expect["encodedWithout"]?.AsArray ?? new List<JsonValue>())
            if (encoded.Contains("\"" + c.AsString! + "\"")) throw new Xunit.Sdk.XunitException($"encoded contains {c.AsString}");
        if (expect["profileStaysSparse"]?.AsBool == true)
            Same(Sorted(given["json"]!["profiles"]!.AsArray![0]), Sorted(config.Document["profiles"]!.AsArray![0]), "sparse profile");
        if (expect["issues"] is { } issues) SameIssues(issues, ConfigValidator.Validate(config));
        return true;
    });

    [Fact]
    public void RuleRegexIssues() => RunCases("domain/rules.cases.json", id => id == "invalid-regex-issue", (id, given, expect) =>
    {
        // A fragment of a version 3 document.
        var json = given["config"]!;
        var full = JsonValue.Of(new[] { ("version", JsonValue.Of(3)) }.Concat(json.AsObject!.Select(m => (m.Key, m.Value))).ToArray());
        SameIssues(expect["issues"]!, ConfigValidator.Validate(Decode(full)));
        return true;
    });

    [Fact]
    public void TypedViewsFillDefaults()
    {
        var config = Decode(JsonValue.Parse("""
            {"version": 3,
             "profiles": [{"id": "default", "titles": {"strategy": "longest"}, "naming": {"layout": "mediaServer"}}],
             "drives": [{"id": "left", "match": {"devicePath": "/dev/sr0"}, "profile": "default"}],
             "steps": [{"id": "enc", "kind": "handbrake"}, {"id": "cmd", "kind": "command", "command": {"executable": "/bin/echo"}}],
             "rules": [{"id": "bd", "when": {"formats": ["BR*"]}, "then": {"steps": ["enc"]}}]}
            """)!);
        Assert.Empty(ConfigValidator.Validate(config));
        var steps = Config.Steps(config);
        Assert.True(steps[0].Background);
        Assert.Equal("H.265 MKV 1080p30", steps[0].Handbrake!.Preset);
        Assert.False(steps[1].Background);
        Assert.Equal("{outputDir}", steps[1].Command!.Arguments);
        Assert.Equal("left", Config.Drives(config)[0].Id);
        Assert.Equal(new[] { "BR*" }, Config.Rules(config)[0].When.Formats);
        var profile = Config.Profiles(config)[0];
        Assert.Equal(TitleStrategy.Longest, TitleSettings.Decode(profile.Json["titles"]!).Strategy);
        Assert.Equal(Layout.MediaServer, NamingSettings.Decode(profile.Json["naming"]!).Layout);
        // A drive pointing at a profile that doesn't exist, a step used twice, a lone TLS file.
        var bad = Decode(JsonValue.Parse("""
            {"version": 3, "drives": [{"id": "d", "profile": "nope"}], "steps": [{"id": "s", "kind": "command"}, {"id": "s", "kind": "command"}],
             "server": {"tls": {"certificate": "/c.pem"}}}
            """)!);
        var codes = ConfigValidator.Validate(bad).Select(i => MessageCode.Wire(i.Code)).ToList();
        Assert.Equal(new[] { "config.duplicateId", "config.unknownReference", "config.tlsNeedsBoth" }, codes);
    }
}
