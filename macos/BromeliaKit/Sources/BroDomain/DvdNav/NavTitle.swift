/// A disc title: its number, chapter lengths in seconds and the VOB id of each chapter.
public struct NavTitle: Sendable, Equatable {
    public var number: Int
    public var chapters: [Double]
    public var chapterVobs: [Int]

    public init(number: Int, chapters: [Double], chapterVobs: [Int]) {
        self.number = number
        self.chapters = chapters
        self.chapterVobs = chapterVobs
    }
}
