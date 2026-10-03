/// HandBrakeArgs.keepLine's state: the task being encoded (0: none yet) and the next 10 % step to keep. The default
/// value is the state before the first line.
public struct ProgressFilter: Sendable, Equatable {
    public var task: Int
    public var next: Int

    public init(task: Int = 0, next: Int = 0) {
        self.task = task
        self.next = next
    }
}
