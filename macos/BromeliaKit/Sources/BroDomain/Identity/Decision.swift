/// A decision about a disc and why (plan §3.1). Only the media kind is decided this way so far, so the value is a
/// `MediaKind`; the plan's confidence isn't used by anything yet and is left out.
public struct Decision: Sendable, Equatable {
    public var value: MediaKind
    public var reason: BroMessage

    public init(value: MediaKind, reason: BroMessage) {
        self.value = value
        self.reason = reason
    }
}
