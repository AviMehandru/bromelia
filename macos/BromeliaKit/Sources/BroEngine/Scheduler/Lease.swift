import BroFoundation

/// What one acquire gave a step. `Scheduler.release` gives it back, except its drives: they stay with the job until
/// `Scheduler.releaseDrives` or `Scheduler.finish`.
@EngineActor
public final class Lease {
    public let jobId: Id
    /// Leases are numbered in the order they were granted.
    public let seq: Int64
    let counted: [(key: String, count: Int)]
    var released = false

    init(jobId: Id, seq: Int64, counted: [(key: String, count: Int)]) {
        self.jobId = jobId
        self.seq = seq
        self.counted = counted
    }
}
