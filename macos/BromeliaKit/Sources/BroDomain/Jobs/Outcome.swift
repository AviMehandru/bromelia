/// How a job ended (plan §20.2).
public enum Outcome: String, Sendable, CaseIterable, Codable {
    case succeeded
    case succeededWithReadErrors
    case succeededStepsFailed
    case failed
    case cancelled
    case skipped
    case interrupted
}
