import BroDomain
import BroFoundation
import BroPorts

/// A job, as clients see it (dtos.json Job). Optional members are left out of the JSON when absent.
public struct JobDto: Sendable, Equatable {
    public var id: Id
    public var kind: JobKind
    public var state: JobState
    public var outcome: Outcome?
    public var queue: Queue
    public var position: Int?
    public var parentId: Id?
    public var driveId: String?
    public var sessionId: Id?
    public var title: String
    public var mode: String?
    public var automatic: Bool
    public var createdAt: Instant
    public var startedAt: Instant?
    public var finishedAt: Instant?
    public var progress: ProgressDto?
    public var blockedBy: BroMessage?
    public var decision: DecisionDto?
    public var error: BroError?
    public var unitId: Id?
    public var outputPath: String?
    public var fingerprint: String?
    public var counts: JobTotals?

    /// The stored job as clients see it, without what only a running job has (progress, blockedBy, decision, position,
    /// counts).
    public static func of(_ r: JobRecord) -> JobDto {
        JobDto(id: r.id, kind: r.kind, state: r.state, outcome: r.outcome, queue: r.queue, position: nil, parentId: r.parentId, driveId: r.driveId,
               sessionId: r.request.sessionId, title: r.title, mode: r.mode, automatic: r.automatic, createdAt: r.createdAt,
               startedAt: r.startedAt, finishedAt: r.finishedAt, progress: nil, blockedBy: nil, decision: nil, error: r.error, unitId: r.unitId,
               outputPath: nil, fingerprint: r.fingerprint, counts: nil)
    }

    public init(id: Id, kind: JobKind, state: JobState, outcome: Outcome?, queue: Queue, position: Int?, parentId: Id?, driveId: String?,
                sessionId: Id?, title: String, mode: String?, automatic: Bool, createdAt: Instant, startedAt: Instant?, finishedAt: Instant?,
                progress: ProgressDto?, blockedBy: BroMessage?, decision: DecisionDto?, error: BroError?, unitId: Id?, outputPath: String?,
                fingerprint: String?, counts: JobTotals?) {
        self.id = id
        self.kind = kind
        self.state = state
        self.outcome = outcome
        self.queue = queue
        self.position = position
        self.parentId = parentId
        self.driveId = driveId
        self.sessionId = sessionId
        self.title = title
        self.mode = mode
        self.automatic = automatic
        self.createdAt = createdAt
        self.startedAt = startedAt
        self.finishedAt = finishedAt
        self.progress = progress
        self.blockedBy = blockedBy
        self.decision = decision
        self.error = error
        self.unitId = unitId
        self.outputPath = outputPath
        self.fingerprint = fingerprint
        self.counts = counts
    }

    public static func json(_ job: JobDto) -> JsonValue {
        var m: [(key: String, value: JsonValue)] = []
        func add(_ key: String, _ value: JsonValue?) { if let value { m.append((key, value)) } }
        add("id", .string(job.id.value))
        add("kind", .string(job.kind.rawValue))
        add("state", .string(job.state.rawValue))
        add("outcome", job.outcome.map { .string($0.rawValue) })
        add("queue", .string(job.queue.rawValue))
        add("position", job.position.map { .integer(Int64($0)) })
        add("parentId", job.parentId.map { .string($0.value) })
        add("driveId", job.driveId.map { .string($0) })
        add("sessionId", job.sessionId.map { .string($0.value) })
        add("title", .string(job.title))
        add("mode", job.mode.map { .string($0) })
        add("automatic", .bool(job.automatic))
        add("createdAt", .string(Instant.format(job.createdAt)))
        add("startedAt", job.startedAt.map { .string(Instant.format($0)) })
        add("finishedAt", job.finishedAt.map { .string(Instant.format($0)) })
        add("progress", job.progress.map(ProgressDto.json))
        add("blockedBy", job.blockedBy?.toJson())
        add("decision", job.decision.map(DecisionDto.json))
        add("error", job.error.map(JsonDto.error))
        add("unitId", job.unitId.map { .string($0.value) })
        add("outputPath", job.outputPath.map { .string($0) })
        add("fingerprint", job.fingerprint.map { .string($0) })
        add("counts", job.counts.map { c in
            .object([("files", .integer(Int64(c.files))), ("warnings", .integer(Int64(c.warnings))), ("errors", .integer(Int64(c.errors))),
                     ("readErrors", .integer(Int64(c.readErrors)))])
        })
        return .object(m)
    }
}
