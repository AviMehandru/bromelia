import BroFoundation

/// The steps a job runs and what PlanStep decided (stored as `jobs.plan`): the mode, profile and library, plus
/// the rest in `details`.
public struct JobPlan: Sendable, Equatable {
    public var steps: [StepKind]
    public var mode: String?
    public var profileId: String?
    public var libraryId: String?
    public var details: JsonValue?

    public init(steps: [StepKind], mode: String? = nil, profileId: String? = nil, libraryId: String? = nil, details: JsonValue? = nil) {
        self.steps = steps
        self.mode = mode
        self.profileId = profileId
        self.libraryId = libraryId
        self.details = details
    }
}
