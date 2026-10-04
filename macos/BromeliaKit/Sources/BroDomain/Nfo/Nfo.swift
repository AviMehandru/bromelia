/// Kodi / Jellyfin / Emby .nfo documents, written next to a media server library (was MediaServerMetadata).
public enum Nfo {
    static func escape(_ s: String) -> String {
        s.replacingOccurrences(of: "&", with: "&amp;").replacingOccurrences(of: "<", with: "&lt;").replacingOccurrences(of: ">", with: "&gt;")
    }

    /// The root element and one element per non-empty value; an element name may carry attributes.
    static func document(_ root: String, _ elements: [(name: String, value: String)]) -> String {
        var s = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<\(root)>\n"
        for (name, value) in elements where !value.isEmpty {
            let close = name.split(separator: " ").first.map(String.init) ?? name
            s += "  <\(name)>\(escape(value))</\(close)>\n"
        }
        return s + "</\(root)>\n"
    }

    /// Title, year, plot and the ids (the first one is the default).
    static func titled(_ root: String, _ m: Candidate) -> String {
        var e: [(name: String, value: String)] = [("title", m.title), ("year", m.year.map(String.init) ?? ""), ("plot", m.overview)]
        var ids: [(type: String, value: String)] = []
        if let t = m.tmdbId { ids.append(("tmdb", String(t))) }
        if let i = m.imdbId, !i.isEmpty { ids.append(("imdb", i)) }
        for (n, id) in ids.enumerated() { e.append(("uniqueid type=\"\(id.type)\"" + (n == 0 ? " default=\"true\"" : ""), id.value)) }
        return document(root, e)
    }

    /// movie.nfo: title, year, plot and the ids (the first one is the default).
    public static func movie(_ match: Candidate) -> String { titled("movie", match) }

    /// tvshow.nfo: as for a movie.
    public static func show(_ match: Candidate) -> String { titled("tvshow", match) }

    /// An episode's .nfo: title, show, season, episode, plot and air date.
    public static func episode(_ show: String, season: Int, episode: Int, details: EpisodeDetails) -> String {
        document("episodedetails", [("title", details.title), ("showtitle", show), ("season", String(season)), ("episode", String(episode)),
                                    ("plot", details.plot), ("aired", details.aired)])
    }
}
