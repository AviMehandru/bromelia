using System.Collections.Generic;
using System.Threading.Tasks;

namespace Bromelia.Engine;

/// <summary>
/// One reader's bounded queue of client-visible events (the server's SSE connection). The bus fills it on the
/// executor; the reader takes from it on any thread. When an event doesn't fit, everything queued becomes one resync
/// item, and while a resync waits, new events are dropped: the snapshot the reader sends next covers them.
/// </summary>
public sealed class EventStream
{
    readonly object _lock = new();
    readonly Queue<StreamItem> _items = new();
    readonly int _capacity;
    readonly HashSet<EngineEventType>? _types;
    bool _resyncWaiting;
    bool _closed;
    TaskCompletionSource? _waiter;

    internal EventStream(int capacity, IReadOnlyCollection<EngineEventType>? types)
    {
        _capacity = capacity;
        _types = types is { Count: > 0 } ? new HashSet<EngineEventType>(types) : null;
    }

    internal bool Wants(EngineEventType type) => !type.IsInternal() && (_types == null || _types.Contains(type));

    internal void Offer(EngineEvent e)
    {
        lock (_lock)
        {
            if (_closed || _resyncWaiting || !Wants(e.Type)) return;
            if (_items.Count >= _capacity)
            {
                ResyncLocked();
                return;
            }
            _items.Enqueue(new StreamItem.Event(e));
            WakeLocked();
        }
    }

    internal void Resync()
    {
        lock (_lock)
        {
            if (!_closed) ResyncLocked();
        }
    }

    void ResyncLocked()
    {
        _items.Clear();
        _items.Enqueue(StreamItem.Resync.Instance);
        _resyncWaiting = true;
        WakeLocked();
    }

    void WakeLocked()
    {
        var w = _waiter;
        _waiter = null;
        w?.SetResult();
    }

    /// <summary>The next item if one is queued; never waits.</summary>
    public StreamItem? Poll()
    {
        lock (_lock)
        {
            return TakeLocked();
        }
    }

    StreamItem? TakeLocked()
    {
        if (_items.Count == 0) return null;
        var item = _items.Dequeue();
        if (item is StreamItem.Resync) _resyncWaiting = false;
        return item;
    }

    /// <summary>Waits for the next item; null once the stream is closed.</summary>
    public async Task<StreamItem?> Next()
    {
        while (true)
        {
            Task wait;
            lock (_lock)
            {
                if (TakeLocked() is { } item) return item;
                if (_closed) return null;
                _waiter ??= new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
                wait = _waiter.Task;
            }
            await wait.ConfigureAwait(false);
        }
    }

    /// <summary>Ends the stream, from any thread: what was queued is dropped, the bus forgets it, Next returns null.</summary>
    public void Close()
    {
        lock (_lock)
        {
            _closed = true;
            _items.Clear();
            WakeLocked();
        }
    }

    public bool IsClosed
    {
        get
        {
            lock (_lock) return _closed;
        }
    }
}
