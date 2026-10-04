import BroDomain
import BroFoundation

/// Held while the computer must stay awake.
public protocol PowerGuard: Sendable {
    func release()
}
