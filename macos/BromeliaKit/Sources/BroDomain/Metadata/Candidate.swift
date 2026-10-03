/// A movie or show found online: title, year, TMDb and IMDb ids, the provider ("TMDb" or "OMDb"), movie or TV as
/// listed, plot and poster URL ("" when there is none).
public struct Candidate: Sendable, Equatable {
    public var title: String
    public var year: Int?
    public var tmdbId: Int?
    public var imdbId: String?
    public var provider: String
    public var kind: MediaKind?
    public var overview: String
    public var poster: String

    public init(title: String, year: Int?, tmdbId: Int?, imdbId: String?, provider: String, kind: MediaKind? = nil, overview: String = "",
                poster: String = "") {
        self.title = title
        self.year = year
        self.tmdbId = tmdbId
        self.imdbId = imdbId
        self.provider = provider
        self.kind = kind
        self.overview = overview
        self.poster = poster
    }

    /// What to type or pick to choose this candidate: `movie/603`, `tv/1668` or `tt0133093`.
    public static func choice(_ candidate: Candidate) -> String {
        if let id = candidate.tmdbId { return (candidate.kind == .tv ? "tv/" : "movie/") + String(id) }
        return candidate.imdbId ?? ""
    }
}
