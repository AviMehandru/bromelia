import BroDomain
import BroFoundation

/// A row of works: a movie or show.
public struct WorkRecord: Sendable, Equatable {
    public var id: Id
    public var kind: MediaKind
    public var title: String
    public var year: Int?
    public var tmdbId: Int?
    public var imdbId: String?
    public var createdAt: Instant
    public var updatedAt: Instant

    public init(id: Id, kind: MediaKind, title: String, year: Int? = nil, tmdbId: Int? = nil, imdbId: String? = nil,
                createdAt: Instant, updatedAt: Instant) {
        self.id = id
        self.kind = kind
        self.title = title
        self.year = year
        self.tmdbId = tmdbId
        self.imdbId = imdbId
        self.createdAt = createdAt
        self.updatedAt = updatedAt
    }
}
