import BroFoundation

/// What one acquire gave a step. `Scheduler.release` gives it back, except its drives: they stay with the job until
/// `Scheduler.releaseDrives` or `Scheduler.finish`.
@EngineActor
public final class Lease {
    public let jobId: Id
    let counted: [(key: String, count: Int)]
    var released = false

    init(jobId: Id, counted: [(key: String, count: Int)]) {
        self.jobId = jobId
        self.counted = counted
    }
}
