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

    /// Gives the settings back once the run has ended: why they couldn't all be cleaned up (makemkv.keyNotRemoved: the
    /// registration key left in the job's folder; makemkv.registryNotRestored: the user's registry values not back yet),
    /// or nil.
    @discardableResult
    func release() -> BroMessage?
}
