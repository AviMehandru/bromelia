import BroDomain
import BroFoundation

/// The data of a queueChanged event (shared/schema/events.json).
public struct QueueChange: Sendable, Equatable {
    public var queue: Queue
    public var order: [Id]

    public init(queue: Queue, order: [Id]) {
        self.queue = queue
        self.order = order
    }

    public static func json(_ change: QueueChange) -> JsonValue {
        .object([("queue", .string(change.queue.rawValue)), ("order", .array(change.order.map { .string($0.value) }))])
    }
}
