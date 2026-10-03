/// An archived disc of a TV show (from its archive record): its place in the set, its highest episode and its
/// folder.
public struct ArchivedDisc: Sendable, Equatable {
    public var name: String
    public var labelTitle: String
    public var season: Int?
    public var part: Int?
    public var volume: Int?
    public var disc: Int?
    public var lastEpisode: Int?
    public var folder: String

    public init(name: String, labelTitle: String, season: Int?, part: Int?, volume: Int?, disc: Int?, lastEpisode: Int?, folder: String) {
        self.name = name
        self.labelTitle = labelTitle
        self.season = season
        self.part = part
        self.volume = volume
        self.disc = disc
        self.lastEpisode = lastEpisode
        self.folder = folder
    }
}
