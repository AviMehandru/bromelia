namespace Bromelia.Domain;

public enum StepState
{
    Pending,
    Running,
    Succeeded,
    Failed,
    Skipped,
    Cancelled,
    Interrupted,
}
