/// What a step can do after the engine stopped while it ran (plan §12.1, §20.4).
public enum Resumability: String, Sendable, CaseIterable, Codable {
    case idempotent
    case resumeFrom
    case notResumable
}
