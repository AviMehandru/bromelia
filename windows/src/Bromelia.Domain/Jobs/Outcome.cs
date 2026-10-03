namespace Bromelia.Domain;

/// <summary>How a job ended (plan §20.2).</summary>
public enum Outcome
{
    Succeeded,
    SucceededWithReadErrors,
    SucceededStepsFailed,
    Failed,
    Cancelled,
    Skipped,
    Interrupted,
}
