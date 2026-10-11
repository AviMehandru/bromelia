import BroFoundation

/// Why a request isn't granted yet: waiting for room, or blocked by a reason (library.offline).
public enum Wait: Sendable, Equatable {
    case waiting
    case blocked(BroError)
}
