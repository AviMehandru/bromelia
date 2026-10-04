import BroDomain
import BroFoundation

/// The job_steps table.
public protocol StepRepository: Sendable {
    /// Records how step seq ended.
    func save(_ jobId: Id, seq: Int, result: StepResult) async throws(BroError)

    /// The steps that succeeded, in order.
    func completed(_ jobId: Id) async throws(BroError) -> [StepRecord]

    /// Every step, in order.
    func load(_ jobId: Id) async throws(BroError) -> [StepRecord]
}
