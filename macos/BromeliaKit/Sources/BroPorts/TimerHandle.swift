import BroDomain
import BroFoundation

/// A timer; cancel stops it.
public protocol TimerHandle: Sendable {
    func cancel()
}
