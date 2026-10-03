using System.Collections.Generic;
using Bromelia.Foundation;
using static Bromelia.Domain.MetadataJson;

namespace Bromelia.Domain;

/// <summary>OMDb requests (the key goes into the query).</summary>
public static class OmdbRequests
{
    private static HttpRequestSpec Get(string query, string key) =>
        new("GET", "https://www.omdbapi.com/?apikey=" + Escape(key.Trim()) + "&" + query, new List<KeyValuePair<string, string>>());

    /// <summary>The search for <paramref name="name"/>, optionally released in <paramref name="year"/>.</summary>
    public static HttpRequestSpec Search(string name, MediaKind kind, int? year, string key) =>
        Get("s=" + Escape(name) + "&type=" + (kind == MediaKind.Tv ? "series" : "movie") + (year is { } y ? "&y=" + Num(y) : ""), key);

    /// <summary>The movie or show with this IMDb id, with its full plot; null for a TMDb id (OMDb takes IMDb ids
    /// only).</summary>
    public static HttpRequestSpec? Details(OnlineId id, string key) => id is OnlineId.Imdb imdb ? Get("i=" + Escape(imdb.Id) + "&plot=full", key) : null;

    /// <summary>The episodes of <paramref name="season"/> of the show <paramref name="match"/> (null without an IMDb id).</summary>
    public static HttpRequestSpec? Season(Candidate match, int season, string key) =>
        match.ImdbId is { } tt ? Get("i=" + Escape(tt) + "&Season=" + Num(season), key) : null;
}
