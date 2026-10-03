import BroFoundation

/// What a finished step contributes to its job's outcome: its state and error, the read errors it saw, whether it
/// put the unit in quarantine, the MakeMKV notice that explains a failure, the reason to skip the job (a disc
/// archived before), and whether its failure counts (post-processing steps).
public struct StepResult: Sendable, Equatable {
    public var kind: StepKind
    public var state: StepState
    public var error: BroError?
    public var readErrors: Int
    public var quarantined: Bool
    public var notice: MakemkvNotice?
    public var skip: BroMessage?
    public var affectsOutcome: Bool

    public init(kind: StepKind, state: StepState, error: BroError? = nil, readErrors: Int = 0, quarantined: Bool = false,
                notice: MakemkvNotice? = nil, skip: BroMessage? = nil, affectsOutcome: Bool = true) {
        self.kind = kind
        self.state = state
        self.error = error
        self.readErrors = readErrors
        self.quarantined = quarantined
        self.notice = notice
        self.skip = skip
        self.affectsOutcome = affectsOutcome
    }
}
