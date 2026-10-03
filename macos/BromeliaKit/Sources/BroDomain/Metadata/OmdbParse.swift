import BroFoundation

/// OMDb's answers.
public enum OmdbParse {
    static func match(_ r: JsonValue, _ kind: MediaKind?) -> Candidate? {
        guard let title = r["Title"]?.string, !title.isEmpty else { return nil }
        let type = r["Type"]?.string
        return Candidate(title: title, year: MetadataJson.year(r["Year"]), tmdbId: nil, imdbId: r["imdbID"]?.string, provider: "OMDb",
                         kind: type == "series" ? .tv : type == "movie" ? .movie : kind, overview: MetadataJson.text(r["Plot"]),
                         poster: MetadataJson.text(r["Poster"]))
    }

    static func answer(_ bytes: [UInt8]) -> JsonValue? {
        guard let o = MetadataJson.object(bytes), o["Response"]?.string == "True" else { return nil }
        return o
    }

    /// The results of a search (or the one title OMDb answered with), in OMDb's order; `kind` when a result doesn't
    /// say.
    public static func candidates(_ bytes: [UInt8], kind: MediaKind?) -> [Candidate] {
        guard let o = answer(bytes) else { return [] }
        if let search = o["Search"]?.array { return search.filter { $0.members != nil }.compactMap { match($0, kind) } }
        return match(o, kind).map { [$0] } ?? []
    }

    /// A movie or show read by its IMDb id.
    public static func details(_ bytes: [UInt8], kind: MediaKind?) -> Candidate? { answer(bytes).flatMap { match($0, kind) } }

    /// Episode number → title and air date (OMDb lists no plots here).
    public static func season(_ bytes: [UInt8]) -> [Int: EpisodeDetails] {
        var d: [Int: EpisodeDetails] = [:]
        for e in MetadataJson.objects(MetadataJson.object(bytes)?["Episodes"]) {
            if let s = e["Episode"]?.string, !s.isEmpty, s.utf8.allSatisfy({ (48...57).contains($0) }), let n = Int(s),
               let t = e["Title"]?.string, !t.isEmpty {
                d[n] = EpisodeDetails(title: t, aired: MetadataJson.text(e["Released"]))
            }
        }
        return d
    }
}
