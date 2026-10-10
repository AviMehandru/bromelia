/// What an `EventStream` yields: an event, or resync (events were lost: send a snapshot).
public enum StreamItem: Sendable, Equatable {
    case event(EngineEvent)
    case resync
}
