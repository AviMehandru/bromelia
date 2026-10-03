/// Problems (the file must not be trusted: a wrong or truncated title) and notes (differences worth logging that
/// don't make the file unusable) of RipCheck.check.
public struct RipCheckResult: Sendable, Equatable {
    public var problems: [BroMessage]
    public var notes: [BroMessage]

    public init(problems: [BroMessage], notes: [BroMessage]) {
        self.problems = problems
        self.notes = notes
    }
}
