import BroDomain
import BroFoundation

/// A problem every UI shows until it is cleared (dtos.json Problem): the message (code, params, severity warning or
/// error), what it is about, when it was first raised and what a UI can offer.
public struct ProblemDto: Sendable, Equatable {
    public var id: String
    public var message: BroMessage
    public var scope: ProblemScope?
    public var raisedAt: Instant
    public var actions: [ProblemAction]

    public init(id: String, message: BroMessage, scope: ProblemScope?, raisedAt: Instant, actions: [ProblemAction]) {
        self.id = id
        self.message = message
        self.scope = scope
        self.raisedAt = raisedAt
        self.actions = actions
    }

    /// Optional members left out when absent or empty.
    public static func json(_ problem: ProblemDto) -> JsonValue {
        var m: [(key: String, value: JsonValue)] = [("id", .string(problem.id)), ("code", .string(problem.message.code.rawValue))]
        if !problem.message.params.isEmpty { m.append(("params", .object(problem.message.params))) }
        m.append(("severity", .string(problem.message.severity.rawValue)))
        if let scope = problem.scope { m.append(("scope", .object([("kind", .string(scope.kind.rawValue)), ("id", .string(scope.id))]))) }
        m.append(("raisedAt", .string(Instant.format(problem.raisedAt))))
        if !problem.actions.isEmpty { m.append(("actions", .array(problem.actions.map { .string($0.rawValue) }))) }
        return .object(m)
    }
}
