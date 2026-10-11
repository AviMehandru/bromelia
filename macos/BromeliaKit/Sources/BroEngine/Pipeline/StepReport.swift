import BroDomain
import BroFoundation

/// What a step's run says: its output data (for the steps after it), the steps it appends (Plan), whether it skipped,
/// what the job's outcome needs (read errors, quarantine, the MakeMKV notice that explains a failure, a reason to skip
/// the whole job, whether its failure counts), and its error when it failed.
public struct StepReport: Sendable, Equatable {
    public var data: JsonValue
    public var summary: BroMessage?
    public var appendSteps: [StepKind]
    public var skipped: Bool
    public var readErrors: Int
    public var quarantined: Bool
    public var notice: MakemkvNotice?
    public var skipJob: BroMessage?
    public var error: BroError?
    public var affectsOutcome: Bool

    public init(data: JsonValue = .object([]), summary: BroMessage? = nil, appendSteps: [StepKind] = [], skipped: Bool = false, readErrors: Int = 0,
                quarantined: Bool = false, notice: MakemkvNotice? = nil, skipJob: BroMessage? = nil, error: BroError? = nil,
                affectsOutcome: Bool = true) {
        self.data = data
        self.summary = summary
        self.appendSteps = appendSteps
        self.skipped = skipped
        self.readErrors = readErrors
        self.quarantined = quarantined
        self.notice = notice
        self.skipJob = skipJob
        self.error = error
        self.affectsOutcome = affectsOutcome
    }
}
