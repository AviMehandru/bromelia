/// How a chapter of a title is reached (the first way found): a menu, a button or a pre-command.
public struct NavJump: Sendable, Equatable {
    public var title: Int
    public var chapter: Int
    public var how: String

    public init(title: Int, chapter: Int, how: String) {
        self.title = title
        self.chapter = chapter
        self.how = how
    }
}
