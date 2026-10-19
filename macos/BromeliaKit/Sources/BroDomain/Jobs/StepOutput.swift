import BroFoundation

/// What a step produced (stored as `job_steps.output`), for the steps after it, and the read errors it saw, so a job
/// resumed after it still ends with them (JobOutcome.decide).
public struct StepOutput: Sendable, Equatable {
    public var kind: StepKind
    public var data: JsonValue
    public var summary: BroMessage?
    public var readErrors: Int

    public init(kind: StepKind, data: JsonValue, summary: BroMessage? = nil, readErrors: Int = 0) {
        self.kind = kind
        self.data = data
        self.summary = summary
        self.readErrors = readErrors
    }
}
