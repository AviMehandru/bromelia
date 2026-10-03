using Bromelia.Domain;
using Bromelia.Foundation;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Domain.Tests;

public class IdentityTests
{
    /// <summary>A listing written in a fixture: a file name, or {attributes, titles} with tracks.</summary>
    internal static Listing ListingFrom(JsonValue v)
    {
        if (v.AsString is { } file) return ListingTests.ListingOf(Text(file));
        Dictionary<int, string> Attrs(JsonValue? a) =>
            (a?.AsObject ?? new List<KeyValuePair<string, JsonValue>>()).ToDictionary(m => int.Parse(m.Key), m => m.Value.AsString!);
        var attrs = Attrs(v["attributes"]);
        var titles = (v["titles"]?.AsArray ?? new List<JsonValue>()).Select(t => TitlesTests.TitleOf(t) with
        {
            Tracks = (t["tracks"]?.AsArray ?? new List<JsonValue>()).Select(k => new Track((int)k["index"]!.AsInteger!,
                EnumWire.Parse<TrackKind>(k["kind"]?.AsString) ?? TrackKind.Unknown, "", "", "", "", false, Attrs(k["attributes"]))).ToList(),
        }).ToList();
        var typeText = attrs.TryGetValue(1, out var tt) ? tt : "";
        return new Listing(attrs.TryGetValue(2, out var n) ? n : attrs.TryGetValue(32, out var vn) ? vn : "", attrs.TryGetValue(32, out var vol) ? vol : "",
            DiscType.Disc, typeText, titles.Count, titles, attrs);
    }

    private static JsonValue OrNull(int? v) => v is { } x ? JsonValue.Of(x) : JsonValue.Null.Instance;

    private static void SameKeys(JsonValue expect, JsonValue actual, string what)
    {
        foreach (var m in expect.AsObject!) Same(m.Value, actual[m.Key], $"{what}.{m.Key}");
    }

    private static JsonValue LabelJson(Label l) => JsonValue.Of(("title", JsonValue.Of(l.Title)), ("season", OrNull(l.Season)), ("part", OrNull(l.Part)),
        ("volume", OrNull(l.Volume)), ("disc", OrNull(l.Disc)), ("looksLikeSeries", JsonValue.Of(l.LooksLikeSeries)));

    private static DiscFlags FlagsOf(JsonValue flags)
    {
        int raw = 0;
        foreach (var f in flags.AsArray!)
            raw |= f.AsString switch { "dvdFiles" => 1, "hdDvdFiles" => 2, "blurayFiles" => 4, "aacsFiles" => 8, "bdsvmFiles" => 16, _ => 0 };
        return new DiscFlags(raw);
    }

    [Fact]
    public void IdentityCases() => RunCases("domain/identity.cases.json", id => !id.StartsWith("drive-"), (id, given, expect) =>
    {
        if (given["label"] is { } label)
        {
            var parsed = LabelParser.Parse(label.AsString!);
            if (given["playAllEpisodes"] is { } n)
            {
                var d = KindHeuristics.Decide(parsed, null, (int)n.AsInteger!);
                Same(expect["kind"]!.AsString, EnumWire.Name(d.Value), "kind");
                Same(expect["reason"], d.Reason.ToJson(), "reason");
            }
            else if (expect["set"] is { } set) Same(set.AsString, LabelParser.SetDescription(parsed), "set");
            else Same(expect, LabelJson(parsed), "label");
            return true;
        }
        if (given["listing"] is { } listingJson)
        {
            var listing = ListingFrom(listingJson);
            if (expect["episodeLike"] is { } like)
            {
                Same(like, JsonValue.Of(KindHeuristics.EpisodeLike(listing.Titles).Select(t => JsonValue.Of(t.Index))), "episode-like");
                var d = KindHeuristics.Decide(LabelParser.Parse(listing.VolumeName), listing, 0);
                Same(expect["kind"]!.AsString, EnumWire.Name(d.Value), "kind");
                Same(expect["reason"], d.Reason.ToJson(), "reason");
            }
            else if (given["encrypted"] is null)
                Same(expect["format"]!.AsString, EnumWire.Name(FormatDetector.Detect(listing, null, null)), "format");
            else
            {
                var identity = Identity.Resolve(new IdentityInputs(listing, "", given["encrypted"]!.AsBool!.Value,
                    NameOverride: given["nameOverride"]?.AsString ?? "", KindOverride: EnumWire.Parse<MediaKind>(given["kindOverride"]?.AsString)));
                var actual = JsonValue.Of(("name", JsonValue.Of(identity.Name)), ("kind", JsonValue.Of(EnumWire.Name(identity.Kind))),
                    ("format", JsonValue.Of(EnumWire.Name(identity.Format))), ("formatCode", JsonValue.Of(identity.FormatCode.Text)),
                    ("set", JsonValue.Of(LabelParser.SetDescription(identity.Label))), ("reason", identity.Reason.ToJson()),
                    ("label", LabelJson(identity.Label)));
                foreach (var m in expect.AsObject!)
                    if (m.Key == "label") SameKeys(m.Value, actual["label"]!, "label");
                    else Same(m.Value, actual[m.Key], m.Key);
            }
            return true;
        }
        if (given["format"] is { } format)
        {
            Same(expect["formatCode"]!.AsString, FormatDetector.Code(EnumWire.Parse<DiscFormat>(format.AsString)!.Value, given["encrypted"]!.AsBool!.Value).Text, "format code");
            return true;
        }
        if (given["numbers"] is { } numbers)
        {
            var first = MenuNumbers.FirstEpisode(numbers.AsArray!.Select(x => (int)x.AsInteger!).ToList(), (int)given["count"]!.AsInteger!);
            Same(expect["firstEpisode"]!.AsInteger, (long?)first, "first episode");
            return true;
        }
        if (given["text"] is { } text)
        {
            Same(expect["numbers"], JsonValue.Of(MenuNumbers.Parse(text.AsString!).Select(n => JsonValue.Of(n))), "numbers");
            return true;
        }
        if (expect["format"] is { } fmt)
        {
            var detected = FormatDetector.Detect(null, given["flags"] is { } flags ? FlagsOf(flags) : null,
                given["indexBdmv"]?.AsString, given["backupHasVideoTs"]?.AsBool ?? false);
            Same(fmt.AsString, EnumWire.Name(detected), "format");
            return true;
        }
        return false;
    });

    /// <summary>shared/fixtures/episode-continuation.json: the records a case can see are the library's (the
    /// output root) and the history's folders; the season folder's file names give its highest episode.</summary>
    [Fact]
    public void EpisodeContinuationCases()
    {
        var doc = Json("episode-continuation.json");
        var records = doc["records"]!.AsArray!.Select(r =>
        {
            var rec = r["record"]!;
            var disc = rec["disc"]!;
            var folder = r["path"]!.AsString!.Substring(0, r["path"]!.AsString!.LastIndexOf('/'));
            var label = LabelParser.Parse(disc["volumeName"]?.AsString is { Length: > 0 } v ? v : disc["label"]?.AsString ?? "");
            var last = rec["episodes"]?.AsArray?.Select(e => (int?)e["episode"]!.AsInteger).Max();
            bool archived = rec["status"]?.AsString is "success" or "errors" && rec["kind"]?.AsString == "tv";
            return (archived, new ArchivedDisc(rec["name"]?.AsString ?? "", label.Title, (int?)disc["season"]?.AsInteger, (int?)disc["part"]?.AsInteger,
                (int?)disc["volume"]?.AsInteger, (int?)disc["disc"]?.AsInteger, last, folder));
        }).Where(x => x.archived).Select(x => x.Item2).ToList();
        var files = doc["files"]!.AsArray!.Select(f => f.AsString!).ToList();
        foreach (var c in doc["cases"]!.AsArray!)
        {
            var q = c["query"]!;
            var folders = c["folders"]?.AsArray?.Select(f => f.AsString!).ToHashSet() ?? new HashSet<string>();
            var visible = records.Where(r => r.Folder.StartsWith("library/") || folders.Contains(r.Folder)).ToList();
            var seasonFolder = c["seasonFolder"]?.AsString;
            int? highest = seasonFolder == null ? null : EpisodeContinuation.HighestInSeason(
                files.Where(f => f.Substring(0, f.LastIndexOf('/')) == seasonFolder).Select(f => f.Substring(f.LastIndexOf('/') + 1)),
                (int)c["season"]!.AsInteger!);
            var query = new ContinuationQuery(q["name"]!.AsString!, q["labelTitle"]!.AsString!, (int?)q["season"]?.AsInteger,
                (int?)q["part"]?.AsInteger, (int?)q["volume"]?.AsInteger, (int)q["disc"]!.AsInteger!, seasonFolder, highest);
            Same(c["expect"]!.AsInteger, (long?)EpisodeContinuation.Choose(query, visible)?.LastEpisode, c["what"]!.AsString!);
        }
    }
}
