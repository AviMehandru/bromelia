import BroFoundation

/// A job's outcome and the error that explains it (nil for a plain success).
public struct Decided: Sendable, Equatable {
    public var outcome: Outcome
    public var error: BroError?

    public init(outcome: Outcome, error: BroError?) {
        self.outcome = outcome
        self.error = error
    }
}
