import BroFoundation
import BroPorts
import Foundation

/// Secrets in memory (the test root's keystore pair: the "system" store and the secrets folder). Thread-safe. `fails`
/// makes every call about a name fail so before it acts.
public final class MemoryKeystore: Keystore, @unchecked Sendable {
    private let lock = NSLock()
    private var stored: [String: String] = [:]
    private var failing: [String: BroError] = [:]
    private let systemStore: Bool

    public init(systemStore: Bool) { self.systemStore = systemStore }

    public func isSystemStore() -> Bool { systemStore }

    /// Calls about `name` fail with `error` (nil: they work again).
    public func fails(_ name: String, _ error: BroError?) { lock.withLock { failing[name] = error } }

    /// The secrets, by name.
    public func values() -> [String: String] { lock.withLock { stored } }

    public func get(_ name: String) throws(BroError) -> String? {
        lock.lock()
        defer { lock.unlock() }
        try check(name)
        return stored[name]
    }

    public func set(_ name: String, value: String) throws(BroError) {
        lock.lock()
        defer { lock.unlock() }
        try check(name)
        stored[name] = value
    }

    public func remove(_ name: String) throws(BroError) {
        lock.lock()
        defer { lock.unlock() }
        try check(name)
        stored.removeValue(forKey: name)
    }

    /// The lock held.
    private func check(_ name: String) throws(BroError) {
        if let error = failing[name] { throw error }
    }
}
