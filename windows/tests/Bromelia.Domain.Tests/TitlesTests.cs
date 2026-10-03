using Bromelia.Domain;
using Bromelia.Foundation;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Domain.Tests;

public class TitlesTests
{
    /// <summary>A title written in a fixture: any of the Title fields, the rest empty.</summary>
    internal static Title TitleOf(JsonValue t)
    {
        var duration = t["duration"]?.AsString ?? "";
        int seconds = (int)(t["durationSeconds"]?.AsInteger ?? (long)(Foundation.Duration.ParseClock(duration)?.Seconds ?? 0));
        return new Title((int)t["index"]!.AsInteger!, (int?)t["sourceTitleId"]?.AsInteger, t["sourceFile"]?.AsString ?? "",
            t["name"]?.AsString ?? "", t["comment"]?.AsString ?? "", duration, seconds, (int)(t["chapters"]?.AsInteger ?? 0),
            t["sizeBytes"]?.AsInteger ?? 0, t["segmentMap"]?.AsString ?? "", t["outputFileName"]?.AsString ?? "",
            (int?)t["angle"]?.AsInteger, new List<Track>(), new Dictionary<int, string>());
    }

    internal static Listing ListingOf(JsonValue titles) =>
        Listing.Empty with { Titles = titles.AsArray!.Select(TitleOf).ToList() };

    internal static TitleSettings SettingsOf(JsonValue r) => new(
        EnumWire.Parse<TitleStrategy>(r["strategy"]?.AsString) ?? TitleStrategy.All,
        (int)(r["longestCount"]?.AsInteger ?? 1),
        r["indexPattern"]?.AsString ?? "",
        EnumWire.Parse<IndexBase>(r["indexBase"]?.AsString) ?? IndexBase.Makemkv,
        (int)(r["minDurationSeconds"]?.AsInteger ?? 0), (int)(r["maxDurationSeconds"]?.AsInteger ?? 0),
        (int)(r["minChapters"]?.AsInteger ?? 0), (int)(r["maxChapters"]?.AsInteger ?? 0),
        (int)(r["minSizeMB"]?.AsInteger ?? 0), (int)(r["maxSizeMB"]?.AsInteger ?? 0),
        r["includePattern"]?.AsString ?? "", r["excludePattern"]?.AsString ?? "",
        r["skipDuplicates"]?.AsBool ?? true, r["skipAlternateAngles"]?.AsBool ?? false,
        (int)(r["maxTitles"]?.AsInteger ?? 0));

    private static List<int> Ints(JsonValue v) => v.AsArray!.Select(x => (int)x.AsInteger!).ToList();

    [Fact]
    public void TitleRulesCases()
    {
        var inputs = Json("domain/titles.cases.json")["inputs"]!;
        RunCases("domain/titles.cases.json", (id, given, expect) =>
        {
            var listing = given["titles"] is { } name ? ListingOf(inputs[name.AsString!]!)
                : ListingTests.ListingOf(Text(given["listing"]!.AsString!));
            var s = TitleRules.Select(listing, SettingsOf(given["rules"]!));
            Same(expect["selected"], JsonValue.Of(s.Indices.Select(i => JsonValue.Of(i))), "selected");
            Same(expect["requiresManualChoice"]?.AsBool ?? false, s.RequiresManualChoice, "requires manual choice");
            if (expect["error"] is { } error) Same(error["code"]!.AsString, s.Error is { } e ? MessageCode.Wire(e.Code) : null, "error");
            else Same(null, s.Error, "error");
            foreach (var r in expect["reasons"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
                Same(r.Value, s.Trace.Single(t => t.Index.ToString() == r.Key).Reason.ToJson(), $"reason of title {r.Key}");
            Assert.Equal(listing.Titles.Count, s.Trace.Count);
            return true;
        });
    }

    [Fact]
    public void OnePassAndSpaceCases() => RunCases("domain/one-pass-and-space.cases.json", (id, given, expect) =>
    {
        if (given["titles"] is { } titles && given["chosen"] is { } chosen)
        {
            var plan = OnePass.Plan(Ints(chosen), ListingOf(titles), (int?)given["current"]?.AsInteger);
            Same(expect["minLength"]!.AsInteger, (long?)plan?.MinLength, "min length");
        }
        else if (given["listing"] is { } listing)
            Same(expect["matches"]!.AsBool, OnePass.Matches(ListingOf(listing), Ints(given["chosen"]!), ListingOf(given["of"]!)), "matches");
        else if (given["bytes"] is { } bytes)
            Same(expect["required"]!.AsInteger, (long?)SpaceEstimate.Required(new Bytes(bytes.AsInteger!.Value)).Count, "required");
        else if (given["titles"] is { } planTitles)
        {
            var need = SpaceEstimate.ForPlan(ListingOf(planTitles).Titles, Ints(given["handPicked"]!), (int?)given["splitTitle"]?.AsInteger);
            Same(expect["bytes"]!.AsInteger, (long?)need.Count, "bytes");
            Same(expect["required"]!.AsInteger, (long?)SpaceEstimate.Required(need).Count, "required");
        }
        else return false;
        return true;
    });

    [Fact]
    public void ListingMatchFindsMovedTitlesAndChangedDiscs()
    {
        Listing L(string volume, params (int Index, int Source, int Seconds, string Map)[] ts) => Listing.Empty with
        {
            VolumeName = volume,
            Titles = ts.Select(t => TitleOf(JsonValue.Of(("index", JsonValue.Of(t.Index)), ("sourceTitleId", JsonValue.Of(t.Source)),
                ("durationSeconds", JsonValue.Of(t.Seconds)), ("segmentMap", JsonValue.Of(t.Map))))).ToList(),
        };
        var opened = L("SHOW", (0, 1, 1300, "1"), (1, 2, 1310, "2"), (2, 3, 1320, "3"));
        var renumbered = L("SHOW", (0, 2, 1310, "2"), (1, 3, 1320, "3"));
        Assert.Null(ListingMatch.DifferentDisc(opened, renumbered));
        Assert.Equal(new Dictionary<int, int> { [1] = 0, [2] = 1 }, ListingMatch.MapTitles(new[] { 1, 2 }, opened, renumbered, new HashSet<int>()));
        var e = Assert.Throws<BroFailure>(() => ListingMatch.MapTitles(new[] { 0 }, opened, renumbered, new HashSet<int>()));
        Assert.Equal("disc.titleGone", e.Error.Code);
        Assert.Equal(MessageCode.DiscChangedVolume, ListingMatch.DifferentDisc(opened, L("OTHER", (0, 1, 1300, "1")))?.Code);
        Assert.Equal(MessageCode.DiscChangedTitles, ListingMatch.DifferentDisc(opened, L("SHOW", (0, 9, 99, "9")))?.Code);
    }
}
