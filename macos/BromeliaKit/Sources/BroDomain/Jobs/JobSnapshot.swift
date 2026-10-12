import BroFoundation

/// An immutable view of a job that a step gets in its context (plan §12.2): its request, its plan and the
/// outputs of the steps before it.
public struct JobSnapshot: Sendable, Equatable {
    public var id: Id
    public var request: JobRequest
    public var plan: JobPlan?
    public var outputs: [StepOutput]

    public init(id: Id, request: JobRequest, plan: JobPlan?, outputs: [StepOutput]) {
        self.id = id
        self.request = request
        self.plan = plan
        self.outputs = outputs
    }

    /// The data of the last output of that kind of step; nil when no such step ran.
    public static func output(_ job: JobSnapshot, _ kind: StepKind) -> JsonValue? { job.outputs.last { $0.kind == kind }?.data }
}
