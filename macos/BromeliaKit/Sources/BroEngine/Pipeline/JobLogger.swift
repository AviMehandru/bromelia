import BroDomain

/// How a step logs, from its worker: a jobLog event at once; jobs/<id>/log.jsonl when the step ends.
public protocol JobLogger: Sendable {
    func log(_ message: BroMessage)
}
