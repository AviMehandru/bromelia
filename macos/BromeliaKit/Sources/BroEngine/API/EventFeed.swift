import BroFoundation
import Foundation

/// What `EngineAPI.events` gives: the snapshot event first when it was opened without a replayable after, then the
/// deltas; after a gap (the stream's resync) a new snapshot instead of the missed deltas; jobLog events only for the
/// jobs asked for. Read from any thread, one reader at a time; stop closes it.
public final class EventFeed: @unchecked Sendable {
    private let stream: EventStream
    private let jobLog: Set<Id>
    private let snapshot: @Sendable () async throws(BroError) -> EngineEvent
    private let lock = NSLock()
    private var first: EngineEvent?

    /// `first`: the snapshot event to give first (nil when replaying from after). `snapshot` builds a new snapshot event
    /// on the executor (after a gap); it fails once the engine stops.
    init(stream: EventStream, jobLog: [Id], first: EngineEvent?, snapshot: @escaping @Sendable () async throws(BroError) -> EngineEvent) {
        self.stream = stream
        self.jobLog = Set(jobLog)
        self.first = first
        self.snapshot = snapshot
    }

    /// The next event (the snapshot first); nil once closed.
    public func next() async throws -> EngineEvent? {
        if stream.isClosed { return nil }
        if let first = lock.withLock({ () -> EngineEvent? in
            defer { self.first = nil }
            return self.first
        }) {
            return first
        }
        while true {
            switch await stream.next() {
            case nil:
                return nil
            case .resync:
                do {
                    return try await snapshot()
                } catch {
                    close() // the engine stopped while the snapshot was asked for
                    return nil
                }
            case .event(let e):
                if e.type == .jobLog && !jobLog.contains(Id(e.data["jobId"]?.string ?? "")) { continue }
                return e
            }
        }
    }

    /// Ends the feed, from any thread: next returns nil from now on.
    public func close() { stream.close() }

    var isClosed: Bool { stream.isClosed }
}
