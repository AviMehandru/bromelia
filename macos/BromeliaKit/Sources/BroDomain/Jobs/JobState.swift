public enum JobState: String, Sendable, CaseIterable, Codable {
    case queued
    case waitingForResources
    case running
    case blocked
    case awaitingDecision
    case finished
}
