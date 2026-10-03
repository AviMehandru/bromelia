public enum StepState: String, Sendable, CaseIterable, Codable {
    case pending
    case running
    case succeeded
    case failed
    case skipped
    case cancelled
    case interrupted
}
