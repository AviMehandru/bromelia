import BroDomain

/// Where a tool reports what makemkvcon says, event by event (progress, messages, the current title). Called on the
/// tool's task.
public protocol RunSink: Sendable {
    func event(_ event: RobotEvent)
}
