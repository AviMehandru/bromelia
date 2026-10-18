import BroDomain
import BroFoundation

/// Which jobs (GET /v1/jobs, owner 2026-10-17): in any of the states, with any of the outcomes (an empty list is any), of
/// a kind, in a queue, on a drive, for a unit, finished after an instant; newest first, a page at a time.
public struct JobQuery: Sendable, Equatable {
    public var states: [JobState]
    public var outcomes: [Outcome]
    public var kind: JobKind?
    public var queue: Queue?
    public var driveId: String?
    public var unitId: Id?
    public var finishedAfter: Instant?
    public var limit: Int
    public var offset: Int

    public init(states: [JobState] = [], outcomes: [Outcome] = [], kind: JobKind? = nil, queue: Queue? = nil, driveId: String? = nil,
                unitId: Id? = nil, finishedAfter: Instant? = nil, limit: Int, offset: Int) {
        self.states = states
        self.outcomes = outcomes
        self.kind = kind
        self.queue = queue
        self.driveId = driveId
        self.unitId = unitId
        self.finishedAfter = finishedAfter
        self.limit = limit
        self.offset = offset
    }
}
