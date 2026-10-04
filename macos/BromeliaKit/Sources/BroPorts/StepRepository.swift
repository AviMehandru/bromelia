import BroDomain
import BroFoundation

/// The job_steps table.
public protocol StepRepository: Sendable {
    /// Writes the step's row (inserted, or replaced for the same job and seq).
    func save(_ step: StepRecord) async throws(BroError)

    /// The steps that succeeded, in order.
    func completed(_ jobId: Id) async throws(BroError) -> [StepRecord]

    /// Every step, in order.
    func load(_ jobId: Id) async throws(BroError) -> [StepRecord]
}
