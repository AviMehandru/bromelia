import BroFoundation

/// How a job ended, from its steps' results (plan §20.3): one pure function instead of today's chain of flags in
/// three run() methods.
public enum JobOutcome {
    /// Steps after the archive is committed: their failures never fail a job.
    static let afterCommit: Set<StepKind> = [.publish, .protect, .postProcess, .notify, .runCommand, .transcode]

    /// In order: cancelled (job.cancelledByUser); interrupted (recovery.interrupted); skipped (the skip reason); a
    /// failed step that isn't after the commit fails the job with its error, or with the MakeMKV problem that
    /// explains it (the error as its cause); read errors give succeededWithReadErrors (rip.readErrors); a failed
    /// post-commit step that affects the outcome gives succeededStepsFailed; else succeeded.
    public static func decide(_ stepResults: [StepResult], cancelled: Bool, policy: OutcomePolicy) -> Decided {
        if cancelled { return Decided(outcome: .cancelled, error: BroMessage(.jobCancelledByUser, severity: .warning).toError()) }
        if let interrupted = stepResults.first(where: { $0.state == .interrupted }) {
            return Decided(outcome: .interrupted, error: BroMessage(.recoveryInterrupted, [("step", .string(interrupted.kind.rawValue))],
                                                                    severity: .warning).toError())
        }
        if let skip = stepResults.first(where: { $0.skip != nil })?.skip { return Decided(outcome: .skipped, error: skip.toError()) }
        if let failed = stepResults.first(where: { $0.state == .failed && !afterCommit.contains($0.kind) }) {
            var error = failed.error ?? BroMessage(.internalUnexpected, severity: .error).toError()
            if let notice = failed.notice, let problem = problemCode(notice) {
                error = BroMessage(problem, severity: .error).toError(cause: error)
            }
            return Decided(outcome: .failed, error: error)
        }
        let readErrors = stepResults.reduce(0) { $0 + $1.readErrors }
        if readErrors > 0 {
            return Decided(outcome: .succeededWithReadErrors,
                           error: BroMessage(.ripReadErrors, [("count", .integer(Int64(readErrors)))], severity: .error).toError())
        }
        if let post = stepResults.first(where: { $0.state == .failed && $0.affectsOutcome }) {
            return Decided(outcome: .succeededStepsFailed, error: post.error)
        }
        return Decided(outcome: .succeeded, error: nil)
    }

    static func problemCode(_ notice: MakemkvNotice) -> MessageCode? {
        switch notice {
        case .keyExpired: return .makemkvKeyExpired
        case .evaluationNotStarted: return .makemkvEvaluationNotStarted
        case .versionTooOld: return .makemkvTooOld
        case .libreDriveRequired: return .makemkvLibreDriveRequired
        case .libreDrive: return nil
        }
    }
}
