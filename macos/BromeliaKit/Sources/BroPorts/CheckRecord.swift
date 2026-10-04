import BroDomain
import BroFoundation

/// A row of checks: one check of a unit, a replica or a folder.
public struct CheckRecord: Sendable, Equatable {
    public var id: Id
    public var unitId: Id?
    public var replicaId: Id?
    public var folder: String
    public var jobId: Id?
    public var startedAt: Instant
    public var finishedAt: Instant?
    public var result: CheckResult
    public var files: Int
    public var bytes: Int64
    public var changed: [String]
    public var unreadable: [String]
    public var missing: [String]
    public var unlisted: [String]
    public var error: BroMessage?

    public init(id: Id, unitId: Id? = nil, replicaId: Id? = nil, folder: String, jobId: Id? = nil, startedAt: Instant,
                finishedAt: Instant? = nil, result: CheckResult, files: Int, bytes: Int64, changed: [String],
                unreadable: [String], missing: [String], unlisted: [String], error: BroMessage? = nil) {
        self.id = id
        self.unitId = unitId
        self.replicaId = replicaId
        self.folder = folder
        self.jobId = jobId
        self.startedAt = startedAt
        self.finishedAt = finishedAt
        self.result = result
        self.files = files
        self.bytes = bytes
        self.changed = changed
        self.unreadable = unreadable
        self.missing = missing
        self.unlisted = unlisted
        self.error = error
    }
}
