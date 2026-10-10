import BroFoundation

/// One event: its seq, when it was published, its type, and its DTO as JSON (shared/schema/events.json).
public struct EngineEvent: Sendable, Equatable {
    public let seq: Int64
    public let at: Instant
    public let type: EngineEventType
    public let data: JsonValue

    public init(seq: Int64, at: Instant, type: EngineEventType, data: JsonValue) {
        self.seq = seq
        self.at = at
        self.type = type
        self.data = data
    }

    /// {seq, at, type, data}: the envelope clients get.
    public static func json(_ event: EngineEvent) -> JsonValue {
        .object([("seq", .integer(event.seq)), ("at", .string(Instant.format(event.at))), ("type", .string(event.type.rawValue)),
                 ("data", event.data)])
    }
}
