/// Where a disc's episode numbering continues: the last episode before it, and where that was found
/// (`episodes.how.previousDisc`'s source: a folder).
public struct PreviousEpisode: Sendable, Equatable {
    public var lastEpisode: Int
    public var source: String

    public init(lastEpisode: Int, source: String) {
        self.lastEpisode = lastEpisode
        self.source = source
    }
}
