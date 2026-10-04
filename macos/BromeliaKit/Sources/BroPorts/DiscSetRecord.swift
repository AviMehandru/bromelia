import BroDomain
import BroFoundation

/// A row of disc_sets: the discs of a release that belong together.
public struct DiscSetRecord: Sendable, Equatable {
    public var id: Id
    public var workId: Id?
    public var labelTitle: String
    public var season: Int?
    public var part: Int?
    public var volume: Int?
    public var description: String
    public var knownCount: Int?

    public init(id: Id, workId: Id? = nil, labelTitle: String, season: Int? = nil, part: Int? = nil,
                volume: Int? = nil, description: String, knownCount: Int? = nil) {
        self.id = id
        self.workId = workId
        self.labelTitle = labelTitle
        self.season = season
        self.part = part
        self.volume = volume
        self.description = description
        self.knownCount = knownCount
    }
}
