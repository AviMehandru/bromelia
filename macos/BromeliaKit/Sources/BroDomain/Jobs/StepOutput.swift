import BroFoundation

/// What a step produced (stored as `job_steps.output`), for the steps after it.
public struct StepOutput: Sendable, Equatable {
    public var kind: StepKind
    public var data: JsonValue
    public var summary: BroMessage?

    public init(kind: StepKind, data: JsonValue, summary: BroMessage? = nil) {
        self.kind = kind
        self.data = data
        self.summary = summary
    }
}
