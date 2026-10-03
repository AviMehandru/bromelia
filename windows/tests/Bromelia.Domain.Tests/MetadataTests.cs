using System.Text;
using Bromelia.Domain;
using Bromelia.Foundation;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Domain.Tests;

public class MetadataTests
{
    private static MediaKind Kind(JsonValue? v) => v?.AsString == "tv" ? MediaKind.Tv : MediaKind.Movie;

    private static int? IntOrNull(JsonValue? v) => v?.AsInteger is { } n ? (int)n : null;

    private static Candidate SeasonMatch(JsonValue s) =>
        new("", null, IntOrNull(s["tmdbId"]), s["imdbId"]?.AsString, "", Kind(s["kind"]));

    /// <summary>The request a fixture case describes; null when the provider can't make it.</summary>
    private static HttpRequestSpec? RequestOf(JsonValue given)
    {
        bool tmdb = given["provider"]!.AsString == "tmdb";
        var key = given["key"]!.AsString!;
        const string language = "en-US";
        if (given["search"] is { } s)
        {
            var year = IntOrNull(s["year"]);
            return tmdb ? TmdbRequests.Search(s["name"]!.AsString!, Kind(s["kind"]), year, key, language)
                : OmdbRequests.Search(s["name"]!.AsString!, Kind(s["kind"]), year, key);
        }
        if (given["id"] is { } idJson)
        {
            OnlineId id = idJson["imdb"] is { } imdb ? new OnlineId.Imdb(imdb.AsString!)
                : new OnlineId.Tmdb((int)idJson["tmdb"]!.AsInteger!, idJson["kind"] == null ? null : Kind(idJson["kind"]));
            if (!tmdb) return OmdbRequests.Details(id, key);
            return id switch
            {
                OnlineId.Tmdb t => TmdbRequests.Details(t.Id, t.Kind ?? Kind(given["kind"]), key, language),
                OnlineId.Imdb i => TmdbRequests.Find(i.Id, key, language),
                _ => null,
            };
        }
        if (given["season"] is { } season)
        {
            var match = SeasonMatch(season);
            int n = (int)season["season"]!.AsInteger!;
            return tmdb ? TmdbRequests.Season(match, n, key, language) : OmdbRequests.Season(match, n, key);
        }
        if (given["episodeGroups"] is { } groups) return TmdbRequests.EpisodeGroups((int)groups["tmdbId"]!.AsInteger!, key, language);
        if (given["episodeGroup"] is { } group) return TmdbRequests.EpisodeGroup(group["id"]!.AsString!, key, language);
        throw new Xunit.Sdk.XunitException("no request in the case");
    }

    /// <summary>A candidate as the fixtures write one: only the fields the expected value names.</summary>
    private static void SameCandidate(JsonValue want, Candidate? got, string what)
    {
        if (want is JsonValue.Null) { Assert.Null(got); return; }
        Assert.NotNull(got);
        foreach (var m in want.AsObject!)
        {
            JsonValue actual = m.Key switch
            {
                "title" => JsonValue.Of(got!.Title),
                "year" => got!.Year is { } y ? JsonValue.Of(y) : JsonValue.Null.Instance,
                "tmdbId" => got!.TmdbId is { } t ? JsonValue.Of(t) : JsonValue.Null.Instance,
                "imdbId" => JsonValue.Of(got!.ImdbId),
                "kind" => got!.Kind is { } k ? JsonValue.Of(EnumWire.Name(k)) : JsonValue.Null.Instance,
                "choice" => JsonValue.Of(Candidate.Choice(got!)),
                "poster" => JsonValue.Of(got!.Poster),
                "overview" => JsonValue.Of(got!.Overview),
                _ => throw new Xunit.Sdk.XunitException("unknown field " + m.Key),
            };
            Same(m.Value, actual, what + "." + m.Key);
        }
    }

    private static byte[] Bytes(string s) => Encoding.UTF8.GetBytes(s);

    private static List<Candidate> Search(JsonValue c, byte[] body)
    {
        var name = c["name"]!.AsString!;
        var list = c["provider"]!.AsString == "tmdb" ? TmdbParse.Candidates(body, Kind(c["kind"])) : OmdbParse.Candidates(body, c["kind"] == null ? null : Kind(c["kind"]));
        return Ranking.Rank(list, name, IntOrNull(c["year"]));
    }

    [Fact]
    public void MetadataCases() => RunCases("domain/metadata.cases.json", (id, given, expect) =>
    {
        if (given["fixture"] is { } fixture)
        {
            RecordedAnswers(Json(fixture.AsString!));
            return true;
        }
        if (given["body"] is { } body)
        {
            SameCandidate(expect["best"]!, Search(given, Bytes(body.AsString!)).FirstOrDefault(), "best");
            return true;
        }
        if (given["file"] is { } file)
        {
            var bytes = File.ReadAllBytes(Path_(file.AsString!));
            if (expect["groupId"] is { } groupId) Same(groupId.AsString, TmdbParse.AbsoluteGroup(bytes), "groupId");
            if (expect["titles"] is { } titles)
            {
                var episodes = TmdbParse.AbsoluteEpisodes(bytes);
                foreach (var m in titles.AsObject!) Same(m.Value.AsString, episodes[int.Parse(m.Key)].Title, "title " + m.Key);
            }
            return true;
        }
        var request = RequestOf(given);
        if (expect.AsObject!.Any(m => m.Key == "request") && expect["request"] is JsonValue.Null) { Assert.Null(request); return true; }
        Assert.NotNull(request);
        var url = new Uri(request!.Url);
        if (expect["method"] is { } method) Same(method.AsString, request.Method, "method");
        if (expect["path"] is { } path) Same(path.AsString, url.AbsolutePath, "path");
        foreach (var q in expect["queryContains"]?.AsArray ?? new List<JsonValue>()) Assert.Contains(q.AsString!, url.Query);
        foreach (var q in expect["queryExcludes"]?.AsArray ?? new List<JsonValue>()) Assert.DoesNotContain(q.AsString!, url.Query);
        if (expect["noHeader"] is { } none) Assert.DoesNotContain(request.Headers, h => h.Key == none.AsString);
        foreach (var h in expect["header"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
            Assert.Contains(new KeyValuePair<string, string>(h.Key, h.Value.AsString!), request.Headers);
        return true;
    });

    private static void RecordedAnswers(JsonValue f)
    {
        foreach (var c in f["search"]!.AsArray!)
        {
            var got = Search(c, File.ReadAllBytes(Path_("lookup/" + c["file"]!.AsString!)));
            var want = c["expect"]!.AsArray!;
            Assert.Equal(want.Count, got.Count);
            for (int i = 0; i < want.Count; i++) SameCandidate(want[i], got[i], c["file"]!.AsString! + "[" + i + "]");
        }
        foreach (var c in f["details"]!.AsArray!)
        {
            var bytes = File.ReadAllBytes(Path_("lookup/" + c["file"]!.AsString!));
            var got = c["provider"]!.AsString == "tmdb" ? TmdbParse.Details(bytes, Kind(c["kind"])) : OmdbParse.Details(bytes, Kind(c["kind"]));
            SameCandidate(c["expect"]!, got, c["file"]!.AsString!);
        }
        foreach (var c in f["season"]!.AsArray!)
        {
            var bytes = File.ReadAllBytes(Path_("lookup/" + c["file"]!.AsString!));
            var got = c["provider"]!.AsString == "tmdb" ? TmdbParse.Season(bytes) : OmdbParse.Season(bytes);
            Assert.Equal(c["expect"]!.AsObject!.Count, got.Count);
            foreach (var m in c["expect"]!.AsObject!) Same(m.Value.AsString, got[int.Parse(m.Key)].Title, "episode " + m.Key);
            foreach (var m in c["aired"]!.AsObject!) Same(m.Value.AsString, got[int.Parse(m.Key)].Aired, "aired " + m.Key);
        }
        foreach (var c in f["ids"]!.AsArray!)
        {
            var id = OnlineId.Parse(c["text"]!.AsString!);
            if (c["invalid"]?.AsBool == true) { Assert.Null(id); continue; }
            if (c["imdb"] is { } imdb) Assert.Equal(new OnlineId.Imdb(imdb.AsString!), id);
            else Assert.Equal(new OnlineId.Tmdb((int)c["tmdb"]!.AsInteger!, c["kind"] == null ? null : Kind(c["kind"])), id);
        }
        foreach (var c in f["splitYear"]!.AsArray!)
            Assert.Equal(new NameAndYear(c["name"]!.AsString!, IntOrNull(c["year"])), OnlineId.SplitYear(c["text"]!.AsString!));
    }
}
