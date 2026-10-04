using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Adapters;

/// <summary>Online lookup (plan §10.2; shared/fixtures/adapters/metadata-client.cases.json): Domain's TMDb and OMDb
/// requests and parsers over the HttpClient. A 200 answer is kept in the lookup cache for 7 days, keyed by the provider
/// and the request without the key. metadata.http for another status; http.failed is passed on; metadata.idUnsupported
/// for an id the provider can't take.</summary>
public sealed class MetadataClient
{
    static readonly Regex KeyParam = new("([?&])(api_key|apikey)=[^&]*&?", RegexOptions.CultureInvariant);
    const long CacheMilliseconds = 7L * 24 * 3600 * 1000;

    readonly IHttpClient _http;
    readonly ILookupCacheRepository? _cache;
    readonly IClock _clock;
    readonly IFileSystem _fs;
    readonly bool _omdb;
    readonly string _key;
    readonly string _language;

    /// <param name="provider">"tmdb" or "omdb" (metadata.provider).</param>
    /// <param name="cache">None: nothing is cached.</param>
    public MetadataClient(IHttpClient http, ILookupCacheRepository? cache, IClock clock, IFileSystem fs, string provider, string key, string language)
    {
        _http = http;
        _cache = cache;
        _clock = clock;
        _fs = fs;
        _omdb = provider == "omdb";
        _key = key;
        _language = language.Length > 0 ? language : "en-US";
    }

    string Provider => _omdb ? "OMDb" : "TMDb";

    /// <summary>Ranked; a year that finds nothing is dropped; OMDb's best result is read again for its plot.</summary>
    public async Task<IReadOnlyList<Candidate>> Search(string name, MediaKind kind, int? year, CancellationToken cancel)
    {
        async Task<List<Candidate>> Find(int? y) => _omdb
            ? OmdbParse.Candidates(await Fetch(OmdbRequests.Search(name, kind, y, _key), cancel).ConfigureAwait(false), kind)
            : TmdbParse.Candidates(await Fetch(TmdbRequests.Search(name, kind, y, _key, _language), cancel).ConfigureAwait(false), kind);
        var list = Ranking.Rank(await Find(year).ConfigureAwait(false), name, year);
        if (list.Count == 0 && year != null) list = Ranking.Rank(await Find(null).ConfigureAwait(false), name, year);
        if (_omdb && list.Count > 0 && list[0].ImdbId is { } tt && OmdbRequests.Details(new OnlineId.Imdb(tt), _key) is { } details
            && OmdbParse.Details(await Fetch(details, cancel).ConfigureAwait(false), kind) is { } full)
            list[0] = full;
        return list;
    }

    public async Task<Candidate?> Lookup(OnlineId id, MediaKind kind, CancellationToken cancel)
    {
        if (_omdb)
        {
            var request = OmdbRequests.Details(id, _key)
                ?? throw new BroFailure(new BroMessage(MessageCode.MetadataIdUnsupported, Severity.Error, ("provider", JsonValue.Of(Provider)),
                    ("id", JsonValue.Of(Shown(id))), ("omdb", JsonValue.Of(true))).ToError());
            return OmdbParse.Details(await Fetch(request, cancel).ConfigureAwait(false), kind);
        }
        var r = id switch
        {
            OnlineId.Tmdb t => TmdbRequests.Details(t.Id, t.Kind ?? kind, _key, _language),
            OnlineId.Imdb i => TmdbRequests.Find(i.Id, _key, _language),
            _ => throw new System.ArgumentOutOfRangeException(nameof(id)),
        };
        return TmdbParse.Details(await Fetch(r, cancel).ConfigureAwait(false), id is OnlineId.Tmdb { Kind: { } k } ? k : kind);
    }

    public async Task<IReadOnlyDictionary<int, EpisodeDetails>> Season(Candidate match, int season, CancellationToken cancel)
    {
        if (_omdb)
            return OmdbRequests.Season(match, season, _key) is { } o ? OmdbParse.Season(await Fetch(o, cancel).ConfigureAwait(false))
                : new Dictionary<int, EpisodeDetails>();
        return TmdbRequests.Season(match, season, _key, _language) is { } t ? TmdbParse.Season(await Fetch(t, cancel).ConfigureAwait(false))
            : new Dictionary<int, EpisodeDetails>();
    }

    /// <summary>TMDb's absolute episode order (an episode group of type 2); empty when there is none or the provider
    /// is OMDb.</summary>
    public async Task<IReadOnlyDictionary<int, EpisodeDetails>> AbsoluteEpisodes(Candidate match, CancellationToken cancel)
    {
        if (_omdb || match.TmdbId is not { } id) return new Dictionary<int, EpisodeDetails>();
        var groups = await Fetch(TmdbRequests.EpisodeGroups(id, _key, _language), cancel).ConfigureAwait(false);
        if (TmdbParse.AbsoluteGroup(groups) is not { } group) return new Dictionary<int, EpisodeDetails>();
        return TmdbParse.AbsoluteEpisodes(await Fetch(TmdbRequests.EpisodeGroup(group, _key, _language), cancel).ConfigureAwait(false));
    }

    /// <summary>Downloads a poster to <paramref name="destination"/> (written atomically, not cached).</summary>
    public async Task Poster(string url, string destination, CancellationToken cancel)
    {
        var response = await _http.Send(new HttpRequestSpec("GET", url, new List<KeyValuePair<string, string>>()), cancel).ConfigureAwait(false);
        if (response.Status != 200) throw Http(response.Status);
        _fs.WriteAtomically(destination, response.Body, 0x1A4);
    }

    async Task<byte[]> Fetch(HttpRequestSpec request, CancellationToken cancel)
    {
        var provider = _omdb ? "omdb" : "tmdb";
        var key = request.Method + " " + KeyParam.Replace(request.Url, m => m.Value.EndsWith("&") ? m.Groups[1].Value : "");
        if (_cache != null && await _cache.Get(provider, key).ConfigureAwait(false) is { } cached) return cached.Body;
        var response = await _http.Send(request, cancel).ConfigureAwait(false);
        if (response.Status != 200) throw Http(response.Status);
        if (_cache != null) await _cache.Put(provider, key, response, new Instant(_clock.Now().UnixMilliseconds + CacheMilliseconds)).ConfigureAwait(false);
        return response.Body;
    }

    BroFailure Http(int status) =>
        new(new BroMessage(MessageCode.MetadataHttp, Severity.Error, ("provider", JsonValue.Of(Provider)), ("status", JsonValue.Of(status))).ToError());

    /// <summary>An id as the user writes it: movie/603, tv/1668, tt0133093.</summary>
    static string Shown(OnlineId id) => id switch
    {
        OnlineId.Tmdb { Kind: MediaKind.Movie } t => "movie/" + t.Id.ToString(CultureInfo.InvariantCulture),
        OnlineId.Tmdb { Kind: MediaKind.Tv } t => "tv/" + t.Id.ToString(CultureInfo.InvariantCulture),
        OnlineId.Tmdb t => t.Id.ToString(CultureInfo.InvariantCulture),
        OnlineId.Imdb i => i.Id,
        _ => "",
    };
}
