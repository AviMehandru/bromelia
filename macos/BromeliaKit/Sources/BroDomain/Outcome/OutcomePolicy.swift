/// Settings that change how a job's outcome is decided. None do yet: today's rules are fixed (read errors
/// quarantine, post-processing failures never fail a job). Kept so profiles can add some without changing
/// `JobOutcome.decide`'s signature.
public struct OutcomePolicy: Sendable, Equatable {
    public init() {}

    public static let `default` = OutcomePolicy()
}
