import BroFoundation
import BroPorts

/// Engine events (plan §11.2), used on the executor. Every event gets the next seq; in-engine subscribers get events
/// in seq order (one published by a handler waits until the current one has reached every subscriber); the last
/// `retain` events are kept so a reconnecting stream can catch up; streams carry client-visible events only.
/// shared/fixtures/engine/event-bus.cases.json.
@EngineActor
public final class EventBus {
    private let clock: any Clock
    private let retain: Int
    private let streamCapacity: Int
    private var retained: [EngineEvent] = []
    private var subscriptions: [Subscription] = []
    private var streams: [EventStream] = []
    private var undelivered: [EngineEvent] = []
    private var delivering = false

    /// The last seq published (firstSeq - 1 before the first).
    public private(set) var lastSeq: Int64

    /// `clock` gives each event its time. `firstSeq` is the first event's seq: a host starts each engine above the
    /// last one's, so a stream asking for an earlier engine's events gets a resync instead of this engine's events
    /// under the same numbers. `retain`: how many recent events a stream can catch up on. `streamCapacity`: how many
    /// items a stream holds before its reader is sent a resync.
    public nonisolated init(clock: any Clock, firstSeq: Int64 = 1, retain: Int = 1024, streamCapacity: Int = 1024) {
        self.clock = clock
        self.retain = retain
        self.streamCapacity = streamCapacity
        lastSeq = firstSeq - 1
    }

    @discardableResult
    public func publish(_ type: EngineEventType, data: JsonValue) -> EngineEvent {
        let event = EngineEvent(seq: lastSeq + 1, at: clock.now(), type: type, data: data)
        lastSeq = event.seq
        retained.append(event)
        if retained.count > retain { retained.removeFirst(retained.count - retain) }
        if !type.isInternal {
            streams.removeAll { $0.isClosed }
            for s in streams { s.offer(event) }
        }
        undelivered.append(event)
        if delivering { return event }
        delivering = true
        defer { delivering = false }
        while !undelivered.isEmpty {
            let next = undelivered.removeFirst()
            for sub in subscriptions where sub.wants(next) { sub.handler(next) }
        }
        return event
    }

    /// `handler` gets each event of `types` (every type, internal ones too, when empty) published from now on.
    public func subscribe(types: [EngineEventType], handler: @escaping @EngineActor (EngineEvent) -> Void) -> Subscription {
        let sub = Subscription(bus: self, types: types, handler: handler, fromSeq: lastSeq + 1)
        subscriptions.append(sub)
        return sub
    }

    func remove(_ sub: Subscription) { subscriptions.removeAll { $0 === sub } }

    /// A stream of the client-visible events of `types` (all when empty). With `after` it first replays the retained
    /// events after that seq; it starts with a resync instead when `after` is beyond `lastSeq`, when events after it
    /// are no longer retained, or when there are more of them than a stream holds.
    public func stream(after: Int64?, types: [EngineEventType]) -> EventStream {
        let stream = EventStream(capacity: streamCapacity, types: types)
        if let after, after != lastSeq {
            let oldest = retained.first?.seq ?? lastSeq + 1
            let replay = retained.filter { $0.seq > after && stream.wants($0.type) }
            if after > lastSeq || after < oldest - 1 || replay.count > streamCapacity {
                stream.resync()
            } else {
                replay.forEach(stream.offer)
            }
        }
        streams.append(stream)
        return stream
    }
}
