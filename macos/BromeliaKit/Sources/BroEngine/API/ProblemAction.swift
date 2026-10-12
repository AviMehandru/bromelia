/// What a UI can offer for a problem (dtos.json Problem.actions).
public enum ProblemAction: String, Sendable, CaseIterable {
    case installBetaKey
    case openSettings
    case attachLibrary
    case repair
    case retry
}
