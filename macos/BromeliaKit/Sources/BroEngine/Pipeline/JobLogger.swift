import BroDomain

/// How a step logs, from its worker: a jobLog event at once; jobs/<id>/log.jsonl soon after, and when the step ends.
public protocol JobLogger: Sendable {
    func log(_ message: BroMessage)
}
