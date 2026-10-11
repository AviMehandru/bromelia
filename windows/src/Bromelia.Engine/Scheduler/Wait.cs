using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>Why a request isn't granted yet: waiting for room, or blocked by a reason (library.offline).</summary>
public abstract record Wait
{
    public sealed record Waiting : Wait
    {
        public static readonly Waiting Instance = new();
    }

    public sealed record Blocked(BroError Reason) : Wait;
}
