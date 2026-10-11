import BroDomain

/// One answer of a decision: its id and its label.
public struct DecisionAnswer: Sendable, Equatable {
    public var id: String
    public var label: BroMessage

    public init(id: String, label: BroMessage) {
        self.id = id
        self.label = label
    }
}
