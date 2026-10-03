/// What a disc label such as ONE_PIECE_S2_P7_D2 says: the title, the place in a set, and whether it looks like part
/// of a series.
public struct Label: Sendable, Equatable {
    public var title: String
    public var season: Int?
    public var part: Int?
    public var volume: Int?
    public var disc: Int?
    public var looksLikeSeries: Bool

    public init(title: String, season: Int? = nil, part: Int? = nil, volume: Int? = nil, disc: Int? = nil, looksLikeSeries: Bool = false) {
        self.title = title
        self.season = season
        self.part = part
        self.volume = volume
        self.disc = disc
        self.looksLikeSeries = looksLikeSeries
    }
}
