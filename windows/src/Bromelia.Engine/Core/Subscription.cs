using System;
using System.Collections.Generic;

namespace Bromelia.Engine;

/// <summary>An in-engine subscriber of the <see cref="EventBus"/>. Used on the executor only.</summary>
public sealed class Subscription
{
    readonly EventBus _bus;
    readonly HashSet<EngineEventType>? _types;

    internal readonly Action<EngineEvent> Handler;
    internal readonly long FromSeq;
    internal bool Cancelled;

    internal Subscription(EventBus bus, IReadOnlyCollection<EngineEventType>? types, Action<EngineEvent> handler, long fromSeq)
    {
        _bus = bus;
        _types = types is { Count: > 0 } ? new HashSet<EngineEventType>(types) : null;
        Handler = handler;
        FromSeq = fromSeq;
    }

    internal bool Wants(EngineEvent e) => !Cancelled && e.Seq >= FromSeq && (_types == null || _types.Contains(e.Type));

    /// <summary>No more deliveries, starting with the event being delivered now.</summary>
    public void Cancel()
    {
        if (Cancelled) return;
        Cancelled = true;
        _bus.Remove(this);
    }
}
