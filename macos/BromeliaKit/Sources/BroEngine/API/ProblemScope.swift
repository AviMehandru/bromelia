/// The thing a problem is about: a library, a drive, a job…, by its id.
public struct ProblemScope: Sendable, Equatable {
    public var kind: ProblemScopeKind
    public var id: String

    public init(kind: ProblemScopeKind, id: String) {
        self.kind = kind
        self.id = id
    }
}
