using System;
using System.Collections.Generic;
using System.Linq;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Engine;

/// <summary>
/// Engine events (plan §11.2), used on the executor only. Every event gets the next seq; in-engine subscribers get
/// events in seq order (one published by a handler waits until the current one has reached every subscriber); the
/// last <c>retain</c> events are kept so a reconnecting stream can catch up; streams carry client-visible events only.
/// shared/fixtures/engine/event-bus.cases.json.
/// </summary>
public sealed class EventBus
{
    readonly IClock _clock;
    readonly int _retain;
    readonly int _streamCapacity;
    readonly Queue<EngineEvent> _retained = new();
    readonly List<Subscription> _subscriptions = new();
    readonly List<EventStream> _streams = new();
    readonly Queue<EngineEvent> _undelivered = new();
    bool _delivering;

    /// <param name="clock">Gives each event its time.</param>
    /// <param name="firstSeq">The first event's seq. A host starts each engine above the last one's, so that a stream
    /// asking for an earlier engine's events gets a resync instead of this engine's events under the same numbers.</param>
    /// <param name="retain">How many recent events a stream can catch up on.</param>
    /// <param name="streamCapacity">How many items a stream holds before its reader is sent a resync.</param>
    public EventBus(IClock clock, long firstSeq = 1, int retain = 1024, int streamCapacity = 1024)
    {
        _clock = clock;
        _retain = retain;
        _streamCapacity = streamCapacity;
        LastSeq = firstSeq - 1;
    }

    /// <summary>The last seq published (firstSeq - 1 before the first).</summary>
    public long LastSeq { get; private set; }

    public EngineEvent Publish(EngineEventType type, JsonValue data)
    {
        var e = new EngineEvent(LastSeq + 1, _clock.Now(), type, data);
        LastSeq = e.Seq;
        _retained.Enqueue(e);
        while (_retained.Count > _retain) _retained.Dequeue();
        if (!type.IsInternal())
        {
            _streams.RemoveAll(s => s.IsClosed);
            foreach (var s in _streams) s.Offer(e);
        }
        _undelivered.Enqueue(e);
        if (_delivering) return e;
        _delivering = true;
        try
        {
            while (_undelivered.TryDequeue(out var next))
                foreach (var sub in _subscriptions.ToArray())
                    if (sub.Wants(next)) sub.Handler(next);
        }
        finally
        {
            _delivering = false;
        }
        return e;
    }

    /// <summary><paramref name="handler"/> gets each event of <paramref name="types"/> (every type, internal ones too,
    /// when null or empty) published from now on.</summary>
    public Subscription Subscribe(IReadOnlyCollection<EngineEventType>? types, Action<EngineEvent> handler)
    {
        var sub = new Subscription(this, types, handler, LastSeq + 1);
        _subscriptions.Add(sub);
        return sub;
    }

    internal void Remove(Subscription sub) => _subscriptions.Remove(sub);

    /// <summary>A stream of the client-visible events of <paramref name="types"/> (all when null or empty). With
    /// <paramref name="after"/> it first replays the retained events after that seq; it starts with a resync instead
    /// when <paramref name="after"/> is beyond <see cref="LastSeq"/>, when events after it are no longer retained, or
    /// when there are more of them than a stream holds.</summary>
    public EventStream Stream(long? after, IReadOnlyCollection<EngineEventType>? types)
    {
        var stream = new EventStream(_streamCapacity, types);
        if (after is { } a && a != LastSeq)
        {
            long oldest = _retained.Count > 0 ? _retained.Peek().Seq : LastSeq + 1;
            var replay = _retained.Where(e => e.Seq > a && stream.Wants(e.Type)).ToList();
            if (a > LastSeq || a < oldest - 1 || replay.Count > _streamCapacity)
                stream.Resync();
            else
                foreach (var e in replay) stream.Offer(e);
        }
        _streams.Add(stream);
        return stream;
    }
}
