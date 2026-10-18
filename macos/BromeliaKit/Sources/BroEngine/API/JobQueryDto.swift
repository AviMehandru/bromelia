import BroDomain
import BroFoundation

/// Which jobs a client asks for (dtos.json JobQuery, the query string of GET /v1/jobs): any of the states, any of the
/// outcomes (empty: any), a kind, a queue, a drive, a unit; a page of at most `limit` (1–500) from `cursor` (the last
/// page's next).
public struct JobQueryDto: Sendable, Equatable {
    public static let defaultLimit = 50
    public static let maxLimit = 500

    public var states: [JobState]
    public var outcomes: [Outcome]
    public var kind: JobKind?
    public var queue: Queue?
    public var driveId: String?
    public var unitId: Id?
    public var cursor: String?
    public var limit: Int

    public init(states: [JobState] = [], outcomes: [Outcome] = [], kind: JobKind? = nil, queue: Queue? = nil, driveId: String? = nil,
                unitId: Id? = nil, cursor: String? = nil, limit: Int = JobQueryDto.defaultLimit) {
        self.states = states
        self.outcomes = outcomes
        self.kind = kind
        self.queue = queue
        self.driveId = driveId
        self.unitId = unitId
        self.cursor = cursor
        self.limit = limit
    }
}
