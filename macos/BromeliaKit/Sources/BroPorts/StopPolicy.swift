/// How a process is stopped: TERM first (makemkvcon ignores INT), or INT first (the others).
public enum StopPolicy: String, Sendable, CaseIterable {
    case terminateFirst
    case interruptFirst
}
