import BroDomain
import BroFoundation

/// One step of a job's plan as JobDetail lists it (dtos.json JobDetail.steps): its state, times, error and summary from
/// the step's stored row; pending, with nothing else, when it has none.
public struct JobStepDto: Sendable, Equatable {
    public var seq: Int
    public var kind: StepKind
    public var state: StepState
    public var startedAt: Instant?
    public var finishedAt: Instant?
    public var error: BroError?
    public var summary: BroMessage?

    public init(seq: Int, kind: StepKind, state: StepState, startedAt: Instant? = nil, finishedAt: Instant? = nil, error: BroError? = nil,
                summary: BroMessage? = nil) {
        self.seq = seq
        self.kind = kind
        self.state = state
        self.startedAt = startedAt
        self.finishedAt = finishedAt
        self.error = error
        self.summary = summary
    }
}
