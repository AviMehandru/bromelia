import BroDomain
import BroFoundation

/// A started process. stop escalates (INT →) TERM → KILL, 5 s apart, and abandons the process 30 s after KILL.
public protocol RunningProcess: Sendable {
    /// Its output lines, in order, until it closes its output.
    func lines() -> AsyncStream<OutputLine>

    /// Waits for it to end.
    func wait() async -> ProcessExit

    /// Stops it (see above); wait reports the reason.
    func stop(_ reason: StopReason)
}
