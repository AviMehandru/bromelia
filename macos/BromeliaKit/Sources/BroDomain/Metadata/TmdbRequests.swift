import BroFoundation

/// TMDb requests. A v3 API key (40 characters or fewer) goes into the query; a longer key is a read access token,
/// sent as a Bearer header.
public enum TmdbRequests {
    static func get(_ path: String, _ query: String, _ key: String, _ language: String) -> HttpRequestSpec {
        let k = key.trimmingCharacters(in: .whitespaces)
        let url = "https://api.themoviedb.org/3/" + path + "?" + query + (query.isEmpty ? "" : "&") + "language="
            + MetadataJson.escape(language.isEmpty ? "en-US" : language)
        if k.count <= 40 { return HttpRequestSpec(method: "GET", url: url + "&api_key=" + MetadataJson.escape(k), headers: []) }
        return HttpRequestSpec(method: "GET", url: url, headers: [("Authorization", "Bearer " + k)])
    }

    /// The search for `name`, optionally released (or first aired) in `year`.
    public static func search(_ name: String, kind: MediaKind, year: Int?, key: String, language: String) -> HttpRequestSpec {
        let yearPart = year.map { "&" + (kind == .tv ? "first_air_date_year" : "year") + "=" + String($0) } ?? ""
        return get("search/" + (kind == .tv ? "tv" : "movie"), "query=" + MetadataJson.escape(name) + yearPart, key, language)
    }

    /// The movie or show with TMDb id `id`.
    public static func details(_ id: Int, kind: MediaKind, key: String, language: String) -> HttpRequestSpec {
        get((kind == .tv ? "tv/" : "movie/") + String(id), "", key, language)
    }

    /// The movie or show with IMDb id `imdbId`.
    public static func find(_ imdbId: String, key: String, language: String) -> HttpRequestSpec {
        get("find/" + MetadataJson.escape(imdbId), "external_source=imdb_id", key, language)
    }

    /// The episodes of `season` of the show `match` (nil without a TMDb id).
    public static func season(_ match: Candidate, season: Int, key: String, language: String) -> HttpRequestSpec? {
        match.tmdbId.map { get("tv/\($0)/season/\(season)", "", key, language) }
    }

    /// The episode groups of a show (one of them may be the absolute order).
    public static func episodeGroups(_ tmdbId: Int, key: String, language: String) -> HttpRequestSpec {
        get("tv/\(tmdbId)/episode_groups", "", key, language)
    }

    /// The episodes of an episode group.
    public static func episodeGroup(_ groupId: String, key: String, language: String) -> HttpRequestSpec {
        get("tv/episode_group/" + MetadataJson.escape(groupId), "", key, language)
    }
}
