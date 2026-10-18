import BroDomain
import BroPorts

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

    /// The queue a job of the kind starts in (plan §21): a job that reads a disc, acquisition; checks and copies of the
    /// archive, maintenance; commands and transcodes, processing.
    public static func queue(_ kind: JobKind) -> Queue {
        switch kind {
        case .videoDisc, .audioCd, .dataDisc, .readErrorRetry: return .acquisition
        case .runCommand, .transcode: return .processing
        default: return .maintenance
        }
    }

    /// The steps a run doesn't do again (plan §12.3): the leading ones, by seq, that succeeded or were skipped, or
    /// failed after the commit (JobOutcome.afterCommit: the runner went on after them).
    public static func done(_ steps: [StepRecord]) -> [StepRecord] {
        var done: [StepRecord] = []
        for s in steps.sorted(by: { $0.seq < $1.seq }) {
            let finished = s.state == .succeeded || s.state == .skipped || (s.state == .failed && JobOutcome.afterCommit(s.kind))
            guard s.seq == done.count, finished else { break }
            done.append(s)
        }
        return done
    }
}
