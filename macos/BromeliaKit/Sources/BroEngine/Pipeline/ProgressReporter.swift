import BroDomain

/// How a step reports its progress, from its worker. The runner coalesces it (one update per 100 ms).
public protocol ProgressReporter: Sendable {
    /// `fraction` of the step (0–1), what it does, the item it works on (MakeMKV's current operation, a file).
    func report(_ fraction: Double, operation: BroMessage, current: String?)
}
