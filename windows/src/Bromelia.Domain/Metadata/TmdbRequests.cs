using System.Collections.Generic;
using Bromelia.Foundation;
using static Bromelia.Domain.MetadataJson;

namespace Bromelia.Domain;

/// <summary>TMDb requests. A v3 API key (40 characters or fewer) goes into the query; a longer key is a read access
/// token, sent as a Bearer header.</summary>
public static class TmdbRequests
{
    private static HttpRequestSpec Get(string path, string query, string key, string language)
    {
        var k = key.Trim();
        var url = "https://api.themoviedb.org/3/" + path + "?" + query + (query.Length > 0 ? "&" : "")
            + "language=" + Escape(language.Length > 0 ? language : "en-US");
        if (k.Length <= 40) return new HttpRequestSpec("GET", url + "&api_key=" + Escape(k), new List<KeyValuePair<string, string>>());
        return new HttpRequestSpec("GET", url, new List<KeyValuePair<string, string>> { new("Authorization", "Bearer " + k) });
    }

    /// <summary>The search for <paramref name="name"/>, optionally released (or first aired) in <paramref name="year"/>.</summary>
    public static HttpRequestSpec Search(string name, MediaKind kind, int? year, string key, string language) =>
        Get("search/" + (kind == MediaKind.Tv ? "tv" : "movie"),
            "query=" + Escape(name) + (year is { } y ? "&" + (kind == MediaKind.Tv ? "first_air_date_year" : "year") + "=" + Num(y) : ""), key, language);

    /// <summary>The movie or show with TMDb id <paramref name="id"/>.</summary>
    public static HttpRequestSpec Details(int id, MediaKind kind, string key, string language) =>
        Get((kind == MediaKind.Tv ? "tv/" : "movie/") + Num(id), "", key, language);

    /// <summary>The movie or show with IMDb id <paramref name="imdbId"/>.</summary>
    public static HttpRequestSpec Find(string imdbId, string key, string language) => Get("find/" + Escape(imdbId), "external_source=imdb_id", key, language);

    /// <summary>The episodes of <paramref name="season"/> of the show <paramref name="match"/> (null without a TMDb id).</summary>
    public static HttpRequestSpec? Season(Candidate match, int season, string key, string language) =>
        match.TmdbId is { } id ? Get("tv/" + Num(id) + "/season/" + Num(season), "", key, language) : null;

    /// <summary>The episode groups of a show (one of them may be the absolute order).</summary>
    public static HttpRequestSpec EpisodeGroups(int tmdbId, string key, string language) => Get("tv/" + Num(tmdbId) + "/episode_groups", "", key, language);

    /// <summary>The episodes of an episode group.</summary>
    public static HttpRequestSpec EpisodeGroup(string groupId, string key, string language) => Get("tv/episode_group/" + Escape(groupId), "", key, language);
}
