/// A notification's title and the lines of its body.
public struct TitleAndBody: Sendable, Equatable {
    public var title: BroMessage
    public var lines: [BroMessage]

    public init(title: BroMessage, lines: [BroMessage]) {
        self.title = title
        self.lines = lines
    }
}
