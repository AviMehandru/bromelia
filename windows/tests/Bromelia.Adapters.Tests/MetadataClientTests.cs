using System.Text;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Microsoft.Data.Sqlite;
using Xunit;
using static Bromelia.Tests.Fixtures;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Adapters.Tests;

/// <summary>An HttpClient that answers by URL: the first rule whose match is part of the URL.</summary>
internal sealed class RoutedHttp : IHttpClient
{
    public readonly List<string> Urls = new();
    readonly IReadOnlyList<JsonValue> _rules;
    public RoutedHttp(IReadOnlyList<JsonValue> rules) { _rules = rules; }

    public Task<HttpResponse> Send(HttpRequestSpec request, CancellationToken cancel)
    {
        Urls.Add(request.Url);
        var rule = _rules.FirstOrDefault(r => request.Url.Contains(r["match"]!.AsString!)) ?? throw new InvalidOperationException("no answer for " + request.Url);
        if (rule["reason"]?.AsString is { } reason) throw new BroFailure(new BroError("http.failed", ("reason", JsonValue.Of(reason))));
        var body = rule["file"]?.AsString is { } f ? File.ReadAllBytes(Path_(f)) : Encoding.UTF8.GetBytes(rule["text"]?.AsString ?? "");
        return Task.FromResult(new HttpResponse((int)(rule["status"]?.AsInteger ?? 200), new List<KeyValuePair<string, string>>(), body));
    }
}

/// <summary>shared/fixtures/adapters/metadata-client.cases.json.</summary>
public sealed class MetadataClientTests : IDisposable
{
    readonly string _dir = Path.Combine(Path.GetTempPath(), "bromelia-metadata-" + Guid.NewGuid().ToString("N"));
    readonly List<SqliteStore> _stores = new();

    public MetadataClientTests() => Directory.CreateDirectory(_dir);

    public void Dispose()
    {
        foreach (var s in _stores) s.Close();
        SqliteConnection.ClearAllPools();
        try { Directory.Delete(_dir, true); } catch (IOException) { }
    }

    static Candidate Match(string choice) => OnlineId.Parse(choice) is OnlineId.Tmdb t ? new Candidate("", null, t.Id, null, "TMDb", t.Kind ?? MediaKind.Tv)
        : new Candidate("", null, null, choice, "OMDb", MediaKind.Tv);

    [Fact]
    public void TheSharedCasesPass()
    {
        int n = 0;
        RunCases("adapters/metadata-client.cases.json", (id, given, expect) =>
        {
            var dbPath = Path.Combine(_dir, $"case{n++}.sqlite");
            var store = SqliteStore.Open(dbPath, new FixedClock());
            _stores.Add(store);
            store.Migrate().GetAwaiter().GetResult();
            var http = new RoutedHttp(given["answers"]!.AsArray!);
            var client = new MetadataClient(http, store.LookupCache(), new FixedClock(), new DiskFileSystem(), given["provider"]!.AsString!,
                "0123456789abcdef0123456789abcdef", "en-US");
            var results = new List<IReadOnlyList<Candidate>>();
            var episodes = new List<IReadOnlyDictionary<int, EpisodeDetails>>();
            var cancel = new CancellationToken();
            try
            {
                foreach (var call in given["calls"]!.AsArray!)
                {
                    var c = call.AsArray!;
                    string S(int i) => c[i].AsString!;
                    switch (S(0))
                    {
                        case "search":
                            results.Add(client.Search(S(1), EnumWire.Parse<MediaKind>(S(2))!.Value, (int?)c[3].AsInteger, cancel).GetAwaiter().GetResult());
                            break;
                        case "lookup":
                            var found = client.Lookup(OnlineId.Parse(S(1))!, EnumWire.Parse<MediaKind>(S(2))!.Value, cancel).GetAwaiter().GetResult();
                            results.Add(found is null ? Array.Empty<Candidate>() : new[] { found });
                            break;
                        case "season": episodes.Add(client.Season(Match(S(1)), (int)c[2].AsInteger!.Value, cancel).GetAwaiter().GetResult()); break;
                        case "absoluteEpisodes": episodes.Add(client.AbsoluteEpisodes(Match(S(1)), cancel).GetAwaiter().GetResult()); break;
                        case "poster": client.Poster(S(1), Path.Combine(_dir, S(2)), cancel).GetAwaiter().GetResult(); break;
                        default: return false;
                    }
                }
                Assert.Null(expect["error"]);
            }
            catch (BroFailure f)
            {
                Assert.NotNull(expect["error"]);
                Assert.Equal(expect["error"]!["code"]!.AsString, f.Error.Code);
                foreach (var m in expect["error"]!["params"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>()) Assert.Equal(m.Value, f.Error.Params[m.Key]);
            }
            if (expect["results"] is { } want)
                for (int i = 0; i < want.AsArray!.Count; i++)
                    for (int k = 0; k < want.AsArray[i].AsArray!.Count; k++)
                    {
                        var w = want.AsArray[i].AsArray![k];
                        var got = results[i][k];
                        if (w["title"]?.AsString is { } title) Assert.Equal(title, got.Title);
                        if (w["year"]?.AsInteger is { } year) Assert.Equal((int?)year, got.Year);
                        if (w["tmdbId"]?.AsInteger is { } tmdb) Assert.Equal((int?)tmdb, got.TmdbId);
                        if (w["overviewStarts"]?.AsString is { } o) Assert.StartsWith(o, got.Overview);
                    }
            if (expect["episodes"] is { } eps)
                for (int i = 0; i < eps.AsArray!.Count; i++)
                    foreach (var m in eps.AsArray[i].AsObject!)
                        if (m.Key == "count") Assert.Equal(m.Value.AsInteger, episodes[i].Count);
                        else Assert.Equal(m.Value.AsString, episodes[i][int.Parse(m.Key)].Title);
            if (expect["requests"]?.AsInteger is { } count) Assert.Equal(count, http.Urls.Count);
            if (expect["lastRequestLacks"]?.AsString is { } lacks) Assert.DoesNotContain(lacks, http.Urls[^1]);
            if (expect["cacheKeyLacks"]?.AsString is { } keyLacks)
            {
                using var db = new SqliteConnection($"Data Source={dbPath};Pooling=False");
                db.Open();
                using var cmd = db.CreateCommand();
                cmd.CommandText = "SELECT request_key FROM lookup_cache";
                using var r = cmd.ExecuteReader();
                Assert.True(r.Read());
                Assert.DoesNotContain(keyLacks, r.GetString(0));
            }
            if (expect["file"] is { } files)
                foreach (var m in files.AsObject!) Assert.Equal(m.Value.AsString, File.ReadAllText(Path.Combine(_dir, m.Key)));
            return true;
        });
    }
}
