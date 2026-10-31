import BroDomain

/// The steps by kind. The composition root registers the standard steps; tests register their own.
public final class StepRegistry: Sendable {
    private let steps: [StepKind: any Step]

    public init(_ steps: [any Step]) {
        var byKind: [StepKind: any Step] = [:]
        for s in steps { byKind[s.kind] = s }
        self.steps = byKind
    }

    public func step(_ kind: StepKind) -> (any Step)? { steps[kind] }

    /// The phase 3 steps; both composition roots use it, so their lists can't drift.
    public static func standard() -> StepRegistry {
        StepRegistry([
            AwaitMediaStep(), ProbeStep(), ReconcileStep(), IdentifyStep(), PlanStep(), DecideStep(), AcquireStep(),
            ReleaseStep(), VerifyRipsStep(), NameStep(), SealStep(), CommitStep(), NotifyStep(), KeepApartStep(),
        ])
    }
}
