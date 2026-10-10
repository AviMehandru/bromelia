/// Every event type: the client-visible ones of shared/schema/events.json (the raw value is the wire form), then the
/// internal ones, which are never streamed.
public enum EngineEventType: String, Sendable, CaseIterable {
    case snapshot
    case driveChanged
    case driveRemoved
    case mediumArrived
    case mediumRemoved
    case sessionChanged
    case sessionClosed
    case jobChanged
    case jobFinished
    case jobLog
    case queueChanged
    case problemRaised
    case problemCleared
    case libraryChanged
    case maintenanceProgress
    case configChanged
    case automationCountdown
    case toolsChanged
    case leaseChanged
    case stepFinished

    /// leaseChanged and stepFinished: in-engine only, never streamed.
    public var isInternal: Bool { self == .leaseChanged || self == .stepFinished }
}
