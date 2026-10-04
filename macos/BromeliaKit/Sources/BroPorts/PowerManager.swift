import BroDomain
import BroFoundation

/// Keeps the computer awake.
public protocol PowerManager: Sendable {
    /// Until the guard is released.
    func inhibit(_ reason: String) throws(BroError) -> any PowerGuard
}
