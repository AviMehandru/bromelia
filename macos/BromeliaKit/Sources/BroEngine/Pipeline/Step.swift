import BroDomain
import BroFoundation

/// One step of a job (plan §12.1). `run` is called on a worker, never on the executor: it may block, checks its
/// context's cancellation token and reports through the context. An expected failure is thrown, or reported in
/// `StepReport.error` (with what the outcome needs).
public protocol Step: Sendable {
    var kind: StepKind { get }
    var resumability: Resumability { get }

    /// What the step holds while it runs: a drive, the acquisition slot, library writes, cpu, io.
    func resources(_ job: JobSnapshot) -> ResourceSet

    func run(_ context: StepContext) async throws(BroError) -> StepReport
}
