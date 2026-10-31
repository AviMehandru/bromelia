import BroDomain
import BroPorts
import Foundation

/// An engine log that keeps its lines (the file adapter writes logs/engine.log). Thread-safe.
public final class RecordingEngineLog: EngineLog, @unchecked Sendable {
    private let lock = NSLock()
    private var kept: [BroMessage] = []

    public init() {}

    public func log(_ message: BroMessage) { lock.withLock { kept.append(message) } }

    public func lines() -> [BroMessage] { lock.withLock { kept } }
}
