import BroDomain
import BroFoundation

/// A row of commit_intents with its items: rolled forward at startup (plan §22.3).
public struct CommitIntent: Sendable, Equatable {
    public var unitId: Id
    public var jobId: Id
    public var staging: String
    public var destination: String
    public var merge: Bool
    public var quarantine: Bool
    public var createdAt: Instant
    public var items: [CommitItem]

    public init(unitId: Id, jobId: Id, staging: String, destination: String, merge: Bool, quarantine: Bool,
                createdAt: Instant, items: [CommitItem]) {
        self.unitId = unitId
        self.jobId = jobId
        self.staging = staging
        self.destination = destination
        self.merge = merge
        self.quarantine = quarantine
        self.createdAt = createdAt
        self.items = items
    }
}
