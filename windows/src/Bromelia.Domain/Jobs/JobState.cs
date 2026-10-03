namespace Bromelia.Domain;

public enum JobState
{
    Queued,
    WaitingForResources,
    Running,
    Blocked,
    AwaitingDecision,
    Finished,
}
