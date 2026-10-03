namespace Bromelia.Domain;

/// <summary>The scheduler's three queues (plan §21).</summary>
public enum Queue
{
    Acquisition,
    Processing,
    Maintenance,
}
