import BroDomain
import BroFoundation

/// Step 18 (plan §20.1), a placeholder in Phase 3: it stays in Plan's list and only reports success; the runner
/// publishes JobFinished. The outbox and the sending come with NotificationService in Phase 5. Data: {}.
public struct NotifyStep: Step {
    public init() {}

    public var kind: StepKind { .notify }

    public var resumability: Resumability { .idempotent }

    public func resources(_ job: JobSnapshot) -> ResourceSet { .none }

    public func run(_ context: StepContext) async throws(BroError) -> StepReport { StepReport(data: .object([])) }
}
