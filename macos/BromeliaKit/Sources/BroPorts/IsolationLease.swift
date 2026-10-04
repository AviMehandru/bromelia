import BroDomain
import BroFoundation

/// One makemkvcon launch's settings: a HOME folder, or registry values until makemkvcon has read them.
public protocol IsolationLease: Sendable {
    /// Variables to add to the process's environment (HOME).
    func environment() -> [String: String]

    /// The profile file for --profile, when there is one.
    func profilePath() -> String?

    /// makemkvcon has read its settings (its first output line).
    func firstOutput()

    func release()
}
