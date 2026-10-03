/// A TV disc whose episode numbering may continue another's: the show's name (after the lookup), the title of its
/// label, its place in the set, and the highest episode already in its media-server season folder (with the folder,
/// for the log).
public struct ContinuationQuery: Sendable, Equatable {
    public var name: String
    public var labelTitle: String
    public var season: Int?
    public var part: Int?
    public var volume: Int?
    public var disc: Int
    public var seasonFolder: String?
    public var seasonFolderHighest: Int?

    public init(name: String, labelTitle: String, season: Int?, part: Int?, volume: Int?, disc: Int, seasonFolder: String? = nil,
                seasonFolderHighest: Int? = nil) {
        self.name = name
        self.labelTitle = labelTitle
        self.season = season
        self.part = part
        self.volume = volume
        self.disc = disc
        self.seasonFolder = seasonFolder
        self.seasonFolderHighest = seasonFolderHighest
    }
}
