namespace Bromelia.Domain;

/// <summary>Which post-processing steps run after a job, and the status word scripts and notifications get.</summary>
public static class StepFilter
{
    /// <summary>An enabled step runs on success (succeeded, or succeeded with a failed post-processing step), on
    /// failure (failed, cancelled, interrupted, or succeeded with read errors), or always; skipped jobs never run
    /// steps.</summary>
    public static bool Applies(StepDefinition step, Outcome outcome)
    {
        if (!step.Enabled || outcome == Outcome.Skipped) return false;
        bool success = outcome is Outcome.Succeeded or Outcome.SucceededStepsFailed;
        return step.RunOn switch
        {
            RunOn.Success => success,
            RunOn.Failure => !success,
            _ => true,
        };
    }

    /// <summary>success (succeeded, or a failed post-processing step: the archive is fine), errors (read errors),
    /// failed (also interrupted), cancelled, skipped.</summary>
    public static StatusWord StatusWord(Outcome outcome) => outcome switch
    {
        Outcome.Succeeded or Outcome.SucceededStepsFailed => Domain.StatusWord.Success,
        Outcome.SucceededWithReadErrors => Domain.StatusWord.Errors,
        Outcome.Cancelled => Domain.StatusWord.Cancelled,
        Outcome.Skipped => Domain.StatusWord.Skipped,
        _ => Domain.StatusWord.Failed,
    };
}
