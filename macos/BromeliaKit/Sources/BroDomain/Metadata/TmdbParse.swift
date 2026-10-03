import BroFoundation

/// TMDb's answers.
public enum TmdbParse {
    static func match(_ r: JsonValue, _ kind: MediaKind) -> Candidate? {
        guard let title = r["title"]?.string ?? r["name"]?.string, !title.isEmpty else { return nil }
        let imdb = MetadataJson.text(r["imdb_id"])
        return Candidate(title: title, year: MetadataJson.year(r["release_date"] ?? r["first_air_date"]), tmdbId: MetadataJson.int(r["id"]),
                         imdbId: imdb.isEmpty ? nil : imdb, provider: "TMDb", kind: kind, overview: MetadataJson.text(r["overview"]),
                         poster: r["poster_path"]?.string.map { "https://image.tmdb.org/t/p/original" + $0 } ?? "")
    }

    /// The results of a search, in TMDb's order.
    public static func candidates(_ bytes: [UInt8], kind: MediaKind) -> [Candidate] {
        MetadataJson.objects(MetadataJson.object(bytes)?["results"]).compactMap { match($0, kind) }
    }

    /// A movie or show read by its id: details, or /find (the kind the id belongs to, whatever the disc was taken
    /// for; `kind` breaks a tie).
    public static func details(_ bytes: [UInt8], kind: MediaKind) -> Candidate? {
        guard let o = MetadataJson.object(bytes) else { return nil }
        if o["movie_results"] != nil || o["tv_results"] != nil {
            let movie = MetadataJson.objects(o["movie_results"]).first, show = MetadataJson.objects(o["tv_results"]).first
            if kind == .tv { return show.flatMap { match($0, .tv) } ?? movie.flatMap { match($0, .movie) } }
            return movie.flatMap { match($0, .movie) } ?? show.flatMap { match($0, .tv) }
        }
        guard o["id"] != nil else { return nil }
        return match(o, o["name"] != nil && o["title"] == nil ? .tv : .movie)
    }

    /// Episode number → title, air date and plot.
    public static func season(_ bytes: [UInt8]) -> [Int: EpisodeDetails] {
        var d: [Int: EpisodeDetails] = [:]
        for e in MetadataJson.objects(MetadataJson.object(bytes)?["episodes"]) {
            if let n = MetadataJson.int(e["episode_number"]), let t = e["name"]?.string, !t.isEmpty {
                d[n] = EpisodeDetails(title: t, aired: MetadataJson.text(e["air_date"]), plot: MetadataJson.text(e["overview"]))
            }
        }
        return d
    }

    /// The id of the show's absolute-order episode group: the first group of type 2; nil when there is none.
    public static func absoluteGroup(_ bytes: [UInt8]) -> String? {
        (MetadataJson.object(bytes)?["results"]?.array ?? []).first { MetadataJson.int($0["type"]) == 2 }?["id"]?.string
    }

    static func byOrder(_ items: [JsonValue]) -> [JsonValue] {
        items.enumerated().sorted { a, b in
            let x = MetadataJson.int(a.element["order"]) ?? 0, y = MetadataJson.int(b.element["order"]) ?? 0
            return x != y ? x < y : a.offset < b.offset
        }.map(\.element)
    }

    /// Absolute episode number → details: an episode's place across the groups (groups by order, episodes by order),
    /// counting from 1.
    public static func absoluteEpisodes(_ bytes: [UInt8]) -> [Int: EpisodeDetails] {
        var d: [Int: EpisodeDetails] = [:]
        var n = 0
        for g in byOrder(MetadataJson.objects(MetadataJson.object(bytes)?["groups"])) {
            for e in byOrder(MetadataJson.objects(g["episodes"])) {
                n += 1
                if let t = e["name"]?.string, !t.isEmpty {
                    d[n] = EpisodeDetails(title: t, aired: MetadataJson.text(e["air_date"]), plot: MetadataJson.text(e["overview"]))
                }
            }
        }
        return d
    }
}
