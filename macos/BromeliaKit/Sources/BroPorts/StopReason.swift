/// Why a process is stopped.
public enum StopReason: String, Sendable, CaseIterable {
    case cancelled
    case stalled
    case timedOut
    case shutdown
}
