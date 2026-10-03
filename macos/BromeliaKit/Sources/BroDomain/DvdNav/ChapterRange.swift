/// The first and last disc chapter of an episode.
public struct ChapterRange: Sendable, Equatable {
    public var first: Int
    public var last: Int

    public init(first: Int, last: Int) {
        self.first = first
        self.last = last
    }
}
