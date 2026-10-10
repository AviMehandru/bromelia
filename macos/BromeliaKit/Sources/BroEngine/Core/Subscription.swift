/// An in-engine subscriber of the `EventBus`.
@EngineActor
public final class Subscription {
    private weak var bus: EventBus?
    private let types: Set<EngineEventType>
    let handler: @EngineActor (EngineEvent) -> Void
    let fromSeq: Int64
    private(set) var cancelled = false

    init(bus: EventBus, types: [EngineEventType], handler: @escaping @EngineActor (EngineEvent) -> Void, fromSeq: Int64) {
        self.bus = bus
        self.types = Set(types)
        self.handler = handler
        self.fromSeq = fromSeq
    }

    func wants(_ event: EngineEvent) -> Bool {
        !cancelled && event.seq >= fromSeq && (types.isEmpty || types.contains(event.type))
    }

    /// No more deliveries, starting with the event being delivered now.
    public func cancel() {
        if cancelled { return }
        cancelled = true
        bus?.remove(self)
    }
}
