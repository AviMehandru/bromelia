using System.Globalization;
using System.Text.Json;
using Bromelia.Core.Config;
using Bromelia.Core.Engine;
using Bromelia.Core.Logic;
using Xunit;

namespace Bromelia.Core.Tests;

/// <summary>shared/fixtures/lookup and shared/fixtures/episode-continuation.json: recorded TMDb / OMDb answers and archive
/// records, with the results every platform must get from them.</summary>
public class LookupTests
{
    static string Dir => Path.Combine(AppContext.BaseDirectory, "fixtures", "lookup");
    static JsonElement Expected() => JsonDocument.Parse(File.ReadAllText(Path.Combine(Dir, "expected.json"))).RootElement;
    static MetadataProvider Provider(JsonElement c) => c.GetProperty("provider").GetString() == "omdb" ? MetadataProvider.Omdb : MetadataProvider.Tmdb;
    static MediaKind Kind(JsonElement e) => e.GetString() == "tv" ? MediaKind.Tv : MediaKind.Movie;
    static int? IntOrNull(JsonElement e) => e.ValueKind == JsonValueKind.Number ? e.GetInt32() : null;

    static void Check(MediaMatch? m, JsonElement expect, string what)
    {
        Assert.True(m != null, what);
        Assert.Equal(expect.GetProperty("title").GetString(), m!.Title);
        if (expect.TryGetProperty("year", out var y)) Assert.True(IntOrNull(y) == m.Year, $"{what}: year {m.Year}");
        if (expect.TryGetProperty("tmdbId", out var t)) Assert.True(IntOrNull(t) == m.TmdbId, $"{what}: tmdbId");
        if (expect.TryGetProperty("imdbId", out var i)) Assert.Equal(i.GetString(), m.ImdbId);
        if (expect.TryGetProperty("kind", out var k)) Assert.Equal(Kind(k), m.Kind);
        if (expect.TryGetProperty("choice", out var c)) Assert.Equal(c.GetString(), m.ChoiceId);
        if (expect.TryGetProperty("poster", out var p)) Assert.Equal(p.GetString(), m.Poster);
        if (expect.TryGetProperty("overview", out var o)) Assert.Equal(o.GetString(), m.Overview);
    }

    [Fact]
    public void RankedSearchResults()
    {
        foreach (var c in Expected().GetProperty("search").EnumerateArray())
        {
            var file = c.GetProperty("file").GetString()!;
            var list = MetadataLookup.Candidates(File.ReadAllText(Path.Combine(Dir, file)), Provider(c), c.GetProperty("name").GetString()!,
                IntOrNull(c.GetProperty("year")), Kind(c.GetProperty("kind")));
            var want = c.GetProperty("expect").EnumerateArray().ToList();
            Assert.Equal(want.Count, list.Count);
            for (int n = 0; n < want.Count; n++) Check(list[n], want[n], $"{file} {c.GetProperty("name")} #{n}");
        }
    }

    [Fact]
    public void DetailsAndSeasons()
    {
        var e = Expected();
        foreach (var c in e.GetProperty("details").EnumerateArray())
        {
            var file = c.GetProperty("file").GetString()!;
            Check(MetadataLookup.Details(File.ReadAllText(Path.Combine(Dir, file)), Provider(c), Kind(c.GetProperty("kind"))), c.GetProperty("expect"), file);
        }
        foreach (var c in e.GetProperty("season").EnumerateArray())
        {
            var file = c.GetProperty("file").GetString()!;
            var eps = MetadataLookup.Season(File.ReadAllText(Path.Combine(Dir, file)), Provider(c));
            var want = c.GetProperty("expect").EnumerateObject().ToList();
            Assert.Equal(want.Count, eps.Count);
            foreach (var w in want) Assert.Equal(w.Value.GetString(), eps[int.Parse(w.Name, CultureInfo.InvariantCulture)].Title);
            foreach (var a in c.GetProperty("aired").EnumerateObject()) Assert.Equal(a.Value.GetString(), eps[int.Parse(a.Name, CultureInfo.InvariantCulture)].Aired);
        }
    }

    [Fact]
    public void IdsAndYears()
    {
        var e = Expected();
        foreach (var c in e.GetProperty("ids").EnumerateArray())
        {
            var text = c.GetProperty("text").GetString()!;
            var id = OnlineId.Parse(text);
            if (c.TryGetProperty("invalid", out _)) Assert.True(id == null, text);
            else if (c.TryGetProperty("imdb", out var tt)) Assert.Equal(new OnlineId(null, null, tt.GetString()), id);
            else Assert.Equal(new OnlineId(c.GetProperty("tmdb").GetInt32(), c.TryGetProperty("kind", out var k) ? Kind(k) : null, null), id);
        }
        foreach (var c in e.GetProperty("splitYear").EnumerateArray())
        {
            var (name, year) = MetadataLookup.SplitYear(c.GetProperty("text").GetString()!);
            Assert.Equal(c.GetProperty("name").GetString(), name);
            Assert.Equal(IntOrNull(c.GetProperty("year")), year);
        }
    }

    [Fact]
    public void Requests()
    {
        var c = new MetadataConfig { Provider = MetadataProvider.Tmdb, ApiKey = "0123456789abcdef0123456789abcdef" };
        var search = MetadataLookup.Request("Friends", MediaKind.Tv, c, 1994)!;
        Assert.Equal("/3/search/tv", search.Url.AbsolutePath);
        Assert.Contains("first_air_date_year=1994", search.Url.Query);
        Assert.Equal("/3/movie/603", MetadataLookup.Request(new OnlineId(603, null, null), MediaKind.Movie, c)!.Url.AbsolutePath);
        Assert.Equal("/3/tv/1668", MetadataLookup.Request(new OnlineId(1668, MediaKind.Tv, null), MediaKind.Movie, c)!.Url.AbsolutePath);
        Assert.Contains("external_source=imdb_id", MetadataLookup.Request(new OnlineId(null, null, "tt0108778"), MediaKind.Tv, c)!.Url.Query);
        var show = new MediaMatch("Friends", 1994, 1668, "tt0108778", "TMDb") { Kind = MediaKind.Tv };
        Assert.Equal("/3/tv/1668/season/2", MetadataLookup.SeasonRequest(show, 2, c)!.Url.AbsolutePath);
        c.Provider = MetadataProvider.Omdb;
        Assert.Null(MetadataLookup.Request(new OnlineId(603, null, null), MediaKind.Movie, c));
        var s = MetadataLookup.SeasonRequest(show, 2, c)!;
        Assert.Contains("i=tt0108778", s.Url.Query);
        Assert.Contains("Season=2", s.Url.Query);
        var o = MetadataLookup.Request("Dune", MediaKind.Movie, c, 1984)!;
        Assert.Contains("s=Dune", o.Url.Query);
        Assert.Contains("y=1984", o.Url.Query);
    }

    [Fact]
    public void EpisodeTitlesInNames()
    {
        var v = MediaIdentity.Resolve(null, "FRIENDS_S2_D1", false).TemplateValues("Rip");
        v["releaseYear"] = "1994"; v["seasonOr1"] = "2"; v["track"] = "Title 1";
        v["episode"] = "Episode 02"; v["episodeNumber"] = "2"; v["episodeTitle"] = "The One with the Breast Milk";
        Assert.Equal("Season 02/Friends (1994) - S02E02 - The One with the Breast Milk", TemplateRenderer.RenderPath(MediaServerNaming.EpisodeTemplate, v));
        Assert.Equal("Friends - Episode 02 - The One with the Breast Milk - Season 2 Disc 1 - Rip - Title 1 - DISC",
            TemplateRenderer.RenderPath(OutputConfig.DefaultFileNameTemplate, v));
        v["episodeTitle"] = "";
        Assert.Equal("Season 02/Friends (1994) - S02E02", TemplateRenderer.RenderPath(MediaServerNaming.EpisodeTemplate, v));
    }

    [Fact]
    public void EpisodeNumberingAcrossDiscs()
    {
        var fixture = JsonDocument.Parse(File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "fixtures", "episode-continuation.json"))).RootElement;
        var baseDir = Path.Combine(Path.GetTempPath(), "bromelia-cont-" + Guid.NewGuid().ToString("N")[..6]);
        try
        {
            foreach (var r in fixture.GetProperty("records").EnumerateArray())
            {
                var path = Path.Combine(baseDir, r.GetProperty("path").GetString()!);
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                File.WriteAllText(path, r.GetProperty("record").GetRawText());
            }
            foreach (var f in fixture.GetProperty("files").EnumerateArray())
            {
                var path = Path.Combine(baseDir, f.GetString()!);
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                File.WriteAllText(path, "");
            }
            foreach (var c in fixture.GetProperty("cases").EnumerateArray())
            {
                var q = c.GetProperty("query");
                int? Opt(string k) => q.TryGetProperty(k, out var v) ? IntOrNull(v) : null;
                var query = new EpisodeContinuation.Query(q.GetProperty("name").GetString()!, q.GetProperty("labelTitle").GetString()!,
                    Opt("season"), Opt("part"), Opt("volume"), q.GetProperty("disc").GetInt32());
                var folders = c.TryGetProperty("folders", out var fs) ? fs.EnumerateArray().Select(x => Path.Combine(baseDir, x.GetString()!)).ToList() : new List<string>();
                var seasonFolder = c.TryGetProperty("seasonFolder", out var sf) ? Path.Combine(baseDir, sf.GetString()!) : null;
                var found = EpisodeContinuation.Find(query, Path.Combine(baseDir, "library"), folders, seasonFolder,
                    c.TryGetProperty("season", out var s) ? s.GetInt32() : 1);
                Assert.True(IntOrNull(c.GetProperty("expect")) == found?.LastEpisode, $"{c.GetProperty("what")}: {found?.LastEpisode}");
            }
        }
        finally { try { Directory.Delete(baseDir, true); } catch (IOException) { } }
    }

    [Fact]
    public void NfoFiles()
    {
        var dir = Path.Combine(AppContext.BaseDirectory, "fixtures", "nfo");
        foreach (var c in JsonDocument.Parse(File.ReadAllText(Path.Combine(dir, "nfo-cases.json"))).RootElement.GetProperty("cases").EnumerateArray())
        {
            var name = c.GetProperty("nfo").GetString()!;
            var want = File.ReadAllText(Path.Combine(dir, name));
            string got;
            if (c.TryGetProperty("details", out var details))
            {
                var m = MetadataLookup.Details(File.ReadAllText(Path.Combine(Dir, details.GetString()!)), Provider(c), Kind(c.GetProperty("kind")))!;
                got = MediaServerMetadata.Nfo(m, Kind(c.GetProperty("kind")));
            }
            else if (c.TryGetProperty("seasonFile", out var season))
            {
                var eps = MetadataLookup.Season(File.ReadAllText(Path.Combine(Dir, season.GetString()!)), Provider(c));
                var n = c.GetProperty("episode").GetInt32();
                got = MediaServerMetadata.EpisodeNfo(c.GetProperty("show").GetString()!, c.GetProperty("season").GetInt32(), n, eps[n]);
            }
            else got = MediaServerMetadata.Nfo(new MediaMatch(c.GetProperty("match").GetProperty("title").GetString()!, null, null, null, "TMDb"), Kind(c.GetProperty("kind")));
            Assert.Equal(want.Replace("\r\n", "\n"), got);
        }
        Assert.True(MediaServerMetadata.IsMetadataFile("Friends (1994) - S02E01.NFO") && MediaServerMetadata.IsMetadataFile("poster.jpg"));
        Assert.False(MediaServerMetadata.IsMetadataFile("Friends (1994) - S02E01.mkv"));
    }
}
