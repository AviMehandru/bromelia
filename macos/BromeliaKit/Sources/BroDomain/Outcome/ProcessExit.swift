import BroFoundation

/// How a process ended: its exit status, the signal that ended it, the silence that stopped it (nil when it didn't
/// stall), whether it was abandoned after KILL (it may still be running and writing: what it wrote to must be
/// quarantined, never reused or removed), and whether it was cancelled (by the user or a shutdown, not by the engine's
/// own decision).
public struct ProcessExit: Sendable, Equatable {
    public var status: Int
    public var signal: Int?
    public var stalled: Duration?
    public var abandoned: Bool
    public var cancelled: Bool

    public init(status: Int, signal: Int? = nil, stalled: Duration? = nil, abandoned: Bool = false, cancelled: Bool = false) {
        self.status = status
        self.signal = signal
        self.stalled = stalled
        self.abandoned = abandoned
        self.cancelled = cancelled
    }
}
