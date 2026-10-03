/// Where the episodes of one title start and end: the first chapter of each episode (1-based), the last chapter of
/// the last one and the rule that found it, the chapters (at least 1 s) played after it, the chapters to split at,
/// the duration and chapter starts in seconds, the chapters kept, each episode's duration, and how each start is
/// reached.
public struct EpisodePlan: Sendable, Equatable {
    public var title: Int
    public var starts: [Int]
    public var lastEnd: Int
    public var endRule: String
    public var tail: [Int]
    public var splitChapters: [Int]
    public var duration: Double
    public var chapterStarts: [Double]
    public var keptChapters: Int
    public var episodeDurations: [Double]
    public var reasons: [String]

    public init(title: Int, starts: [Int], lastEnd: Int, endRule: String, tail: [Int], splitChapters: [Int], duration: Double,
                chapterStarts: [Double], keptChapters: Int, episodeDurations: [Double], reasons: [String]) {
        self.title = title
        self.starts = starts
        self.lastEnd = lastEnd
        self.endRule = endRule
        self.tail = tail
        self.splitChapters = splitChapters
        self.duration = duration
        self.chapterStarts = chapterStarts
        self.keptChapters = keptChapters
        self.episodeDurations = episodeDurations
        self.reasons = reasons
    }
}
