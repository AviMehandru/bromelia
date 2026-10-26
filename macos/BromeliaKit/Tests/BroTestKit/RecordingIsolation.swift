import BroDomain
import BroFoundation
import BroPorts
import Foundation

/// An isolation that records what happens to its leases.
public final class RecordingIsolation: SettingsIsolation, @unchecked Sendable {
    private let lock = NSLock()
    private var entries: [String] = []
    public var log: [String] { lock.withLock { entries } }
    public func add(_ s: String) { lock.withLock { entries.append(s) } }
    /// What release reports.
    public var releaseProblem: BroMessage?
    /// The leases' warning.
    public var warning: BroMessage?

    public init() {}

    public func prepare(_ settings: MakemkvRunSettings) throws(BroError) -> any IsolationLease {
        add("prepare")
        return Lease(owner: self, settings: settings)
    }

    public func recover(_ workDirectories: [String]) -> [BroMessage] {
        add((["recover"] + workDirectories).joined(separator: " "))
        return []
    }

    public struct Lease: IsolationLease {
        public let owner: RecordingIsolation
        public let settings: MakemkvRunSettings
        public func environment() -> [String: String] { ["HOME": settings.workDirectory] }
        public func profilePath() -> String? { settings.profileXml == nil ? nil : settings.workDirectory + "/profile.mmcp.xml" }
        public func warning() -> BroMessage? { owner.warning }
        public func firstOutput() { owner.add("firstOutput") }
        @discardableResult
        public func release() -> BroMessage? {
            owner.add("release")
            return owner.releaseProblem
        }
    }
}
