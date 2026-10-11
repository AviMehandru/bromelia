import BroFoundation

/// How a step asks the user: the job is awaitingDecision until one of the answers is given.
public protocol DecisionPort: Sendable {
    /// The id of the answer; job.cancelled when the job is cancelled first.
    func ask(_ decision: DecisionDto) async throws(BroError) -> String
}
