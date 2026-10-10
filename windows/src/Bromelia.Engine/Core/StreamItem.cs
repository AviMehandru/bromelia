namespace Bromelia.Engine;

/// <summary>What an <see cref="EventStream"/> yields: an event, or resync (events were lost: send a snapshot).</summary>
public abstract record StreamItem
{
    public sealed record Event(EngineEvent Value) : StreamItem;

    public sealed record Resync : StreamItem
    {
        public static readonly Resync Instance = new();
    }
}
