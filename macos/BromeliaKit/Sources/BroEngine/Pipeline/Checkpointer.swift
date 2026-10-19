import BroFoundation

/// What a resumable step keeps between its attempts in one job (plan §12.2), stored with the step's row
/// (job_steps.checkpoint), implemented by the runner. A checkpoint is JSON with a "kind": RecoveryService resumes a
/// resumeFrom step after a restart only from a checkpoint of the kind the step names (`Step.resumeCheckpoint`).
public protocol Checkpointer: Sendable {
    /// What an earlier attempt of the step saved in this job (before a restart, or a retry); nil when none.
    var saved: JsonValue? { get }

    /// Stores `checkpoint` with the step's row; done once it is stored. Any thread.
    func save(_ checkpoint: JsonValue) async throws(BroError)
}
