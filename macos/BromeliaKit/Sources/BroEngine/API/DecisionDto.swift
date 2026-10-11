import BroDomain
import BroFoundation

/// A question a job waits on (dtos.json Decision).
public struct DecisionDto: Sendable, Equatable {
    public var id: String
    public var question: BroMessage
    public var answers: [DecisionAnswer]

    public init(id: String, question: BroMessage, answers: [DecisionAnswer]) {
        self.id = id
        self.question = question
        self.answers = answers
    }

    public static func json(_ decision: DecisionDto) -> JsonValue {
        .object([("id", .string(decision.id)), ("question", decision.question.toJson()),
                 ("answers", .array(decision.answers.map { .object([("id", .string($0.id)), ("label", $0.label.toJson())]) }))])
    }
}
