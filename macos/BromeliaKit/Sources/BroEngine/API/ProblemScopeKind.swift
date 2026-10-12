/// What a problem is about (dtos.json Problem.scope.kind).
public enum ProblemScopeKind: String, Sendable, CaseIterable {
    case library
    case drive
    case job
    case unit
    case tool
    case target
    case notification
    case secret
}
