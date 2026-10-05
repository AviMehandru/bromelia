import BroDomain
import BroFoundation

/// A started process. stop escalates (INT →) TERM → KILL, 5 s apart, and abandons the process 30 s after KILL. An
/// abandoned process (ProcessExit.abandoned) may still be running and writing: whatever it was writing to (a staging
/// folder, an image) must be quarantined, never reused or removed.
public protocol RunningProcess: Sendable {
    /// Its output lines, in order, until it closes its output.
    func lines() -> AsyncStream<OutputLine>

    /// Waits for it to end.
    func wait() async -> ProcessExit

    /// Stops it (see above); wait reports the reason.
    func stop(_ reason: StopReason)

    /// process.noTranscript when the transcript the spec asked for couldn't be opened, or a line couldn't be written to
    /// it (the first failure); nil otherwise.
    func transcriptProblem() -> BroMessage?
}

extension RunningProcess {
    public func transcriptProblem() -> BroMessage? { nil }
}
