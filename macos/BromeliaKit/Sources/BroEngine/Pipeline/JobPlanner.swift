import BroDomain

/// The steps a job starts with (plan §15.2, §20.1); the Plan step appends the rest.
/// shared/fixtures/engine/job-planner.cases.json.
public enum JobPlanner {
    /// A video disc: awaitMedia when the rip is automatic and from a drive, probe, reconcile when a session made the
    /// choices, identify, plan. Kinds whose steps aren't built yet get none.
    public static func initial(_ request: JobRequest) -> [StepKind] {
        guard request.kind == .videoDisc else { return [] }
        var steps: [StepKind] = []
        if request.automatic && request.driveId != nil { steps.append(.awaitMedia) }
        steps.append(.probe)
        if request.sessionId != nil { steps.append(.reconcile) }
        steps += [.identify, .plan]
        return steps
    }
}
