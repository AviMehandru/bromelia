/// Why a process is stopped. Only cancelled and shutdown make its exit cancelled; policy is the engine's own decision
/// (MakeMKV's space warning, a renumbered drive, an error while reading the output).
public enum StopReason: String, Sendable, CaseIterable {
    case cancelled
    case stalled
    case timedOut
    case shutdown
    case policy
}
