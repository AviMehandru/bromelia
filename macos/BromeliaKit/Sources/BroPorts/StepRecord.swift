import BroDomain
import BroFoundation

/// A row of job_steps: the durable checkpoint between steps.
public struct StepRecord: Sendable, Equatable {
    public var jobId: Id
    public var seq: Int
    public var kind: StepKind
    public var state: StepState
    public var attempt: Int
    public var startedAt: Instant?
    public var finishedAt: Instant?
    public var output: StepOutput?
    public var error: BroError?
    /// Resumable steps.
    public var checkpoint: JsonValue?

    public init(jobId: Id, seq: Int, kind: StepKind, state: StepState, attempt: Int, startedAt: Instant? = nil,
                finishedAt: Instant? = nil, output: StepOutput? = nil, error: BroError? = nil,
                checkpoint: JsonValue? = nil) {
        self.jobId = jobId
        self.seq = seq
        self.kind = kind
        self.state = state
        self.attempt = attempt
        self.startedAt = startedAt
        self.finishedAt = finishedAt
        self.output = output
        self.error = error
        self.checkpoint = checkpoint
    }
}
