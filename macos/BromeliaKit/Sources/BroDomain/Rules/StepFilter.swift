/// Which post-processing steps run after a job, and the status word scripts and notifications get.
public enum StepFilter {
    /// An enabled step runs on success (succeeded, or succeeded with a failed post-processing step), on failure
    /// (failed, cancelled, interrupted, or succeeded with read errors), or always; skipped jobs never run steps.
    public static func applies(_ step: StepDefinition, outcome: Outcome) -> Bool {
        guard step.enabled, outcome != .skipped else { return false }
        let success = outcome == .succeeded || outcome == .succeededStepsFailed
        switch step.runOn {
        case .success: return success
        case .failure: return !success
        case .always: return true
        }
    }

    /// success (succeeded, or a failed post-processing step: the archive is fine), errors (read errors), failed
    /// (also interrupted), cancelled, skipped.
    public static func statusWord(_ outcome: Outcome) -> StatusWord {
        switch outcome {
        case .succeeded, .succeededStepsFailed: return .success
        case .succeededWithReadErrors: return .errors
        case .cancelled: return .cancelled
        case .skipped: return .skipped
        case .failed, .interrupted: return .failed
        }
    }
}
