import BroFoundation

/// OMDb requests (the key goes into the query).
public enum OmdbRequests {
    static func get(_ query: String, _ key: String) -> HttpRequestSpec {
        HttpRequestSpec(method: "GET", url: "https://www.omdbapi.com/?apikey=" + MetadataJson.escape(key.trimmingCharacters(in: .whitespaces)) + "&" + query,
                        headers: [])
    }

    /// The search for `name`, optionally released in `year`.
    public static func search(_ name: String, kind: MediaKind, year: Int?, key: String) -> HttpRequestSpec {
        get("s=" + MetadataJson.escape(name) + "&type=" + (kind == .tv ? "series" : "movie") + (year.map { "&y=\($0)" } ?? ""), key)
    }

    /// The movie or show with this IMDb id, with its full plot; nil for a TMDb id (OMDb takes IMDb ids only).
    public static func details(_ id: OnlineId, key: String) -> HttpRequestSpec? {
        guard case .imdb(let tt) = id else { return nil }
        return get("i=" + MetadataJson.escape(tt) + "&plot=full", key)
    }

    /// The episodes of `season` of the show `match` (nil without an IMDb id).
    public static func season(_ match: Candidate, season: Int, key: String) -> HttpRequestSpec? {
        match.imdbId.map { get("i=" + MetadataJson.escape($0) + "&Season=\(season)", key) }
    }
}
