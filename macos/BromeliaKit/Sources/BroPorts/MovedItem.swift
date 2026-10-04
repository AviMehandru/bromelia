import BroDomain
import BroFoundation

/// An item moveMerging moved, and where it went.
public struct MovedItem: Sendable, Equatable {
    public var from: String
    public var to: String

    public init(from: String, to: String) {
        self.from = from
        self.to = to
    }
}
