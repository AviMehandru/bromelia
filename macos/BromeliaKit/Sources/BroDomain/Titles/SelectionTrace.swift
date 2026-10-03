/// Why one title is in or out of a selection.
public struct SelectionTrace: Sendable, Equatable {
    public var index: Int
    public var selected: Bool
    public var reason: BroMessage

    public init(index: Int, selected: Bool, reason: BroMessage) {
        self.index = index
        self.selected = selected
        self.reason = reason
    }
}
