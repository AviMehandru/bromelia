import BroDomain
import BroFoundation

/// A row of jobs.
public struct JobRecord: Sendable, Equatable {
    public var id: Id
    public var kind: JobKind
    public var parentId: Id?
    public var state: JobState
    public var outcome: Outcome?
    public var error: BroError?
    public var blockedBy: JsonValue?
    /// The open question.
    public var decision: JsonValue?
    public var queue: Queue
    public var position: Int
    public var driveId: String?
    public var mediaGeneration: Int64?
    public var automatic: Bool
    public var mode: String?
    public var title: String
    public var fingerprint: String?
    public var request: JobRequest
    public var plan: JobPlan?
    public var unitId: Id?
    public var createdAt: Instant
    public var startedAt: Instant?
    public var finishedAt: Instant?

    public init(id: Id, kind: JobKind, parentId: Id? = nil, state: JobState, outcome: Outcome? = nil,
                error: BroError? = nil, blockedBy: JsonValue? = nil, decision: JsonValue? = nil, queue: Queue,
                position: Int, driveId: String? = nil, mediaGeneration: Int64? = nil, automatic: Bool,
                mode: String? = nil, title: String, fingerprint: String? = nil, request: JobRequest,
                plan: JobPlan? = nil, unitId: Id? = nil, createdAt: Instant, startedAt: Instant? = nil,
                finishedAt: Instant? = nil) {
        self.id = id
        self.kind = kind
        self.parentId = parentId
        self.state = state
        self.outcome = outcome
        self.error = error
        self.blockedBy = blockedBy
        self.decision = decision
        self.queue = queue
        self.position = position
        self.driveId = driveId
        self.mediaGeneration = mediaGeneration
        self.automatic = automatic
        self.mode = mode
        self.title = title
        self.fingerprint = fingerprint
        self.request = request
        self.plan = plan
        self.unitId = unitId
        self.createdAt = createdAt
        self.startedAt = startedAt
        self.finishedAt = finishedAt
    }
}
