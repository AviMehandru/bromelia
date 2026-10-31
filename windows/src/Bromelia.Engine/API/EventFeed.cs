using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>
/// What <see cref="IEngineAPI.Events"/> gives (decision 168): the snapshot event first when it was opened without a
/// replayable after, then the deltas; after a gap (the stream's resync) a new snapshot instead of the missed deltas;
/// jobLog events only for the jobs asked for. Read from any thread, one reader at a time; stop closes it.
/// </summary>
public sealed class EventFeed
{
    readonly EventStream _stream;
    readonly HashSet<Id> _jobLog;
    readonly Func<Task<EngineEvent>> _snapshot;
    EngineEvent? _first;

    /// <param name="first">The snapshot event to give first (null when replaying from after).</param>
    /// <param name="snapshot">Builds a new snapshot event on the executor (after a gap); fails once the engine stops.</param>
    internal EventFeed(EventStream stream, IReadOnlyCollection<Id> jobLog, EngineEvent? first, Func<Task<EngineEvent>> snapshot)
    {
        _stream = stream;
        _jobLog = new HashSet<Id>(jobLog);
        _first = first;
        _snapshot = snapshot;
    }

    /// <summary>The next event (the snapshot first); null once closed.</summary>
    public async Task<EngineEvent?> Next()
    {
        if (_stream.IsClosed) return null;
        if (_first is { } first)
        {
            _first = null;
            return first;
        }
        while (true)
        {
            var item = await _stream.Next().ConfigureAwait(false);
            switch (item)
            {
                case null:
                    return null;
                case StreamItem.Resync:
                    try
                    {
                        return await _snapshot().ConfigureAwait(false);
                    }
                    catch (BroFailure)
                    {
                        Close(); // the engine stopped while the snapshot was asked for
                        return null;
                    }
                case StreamItem.Event { Value: var e }:
                    if (e.Type == EngineEventType.JobLog && !_jobLog.Contains(new Id(e.Data["jobId"]?.AsString ?? ""))) continue;
                    return e;
            }
        }
    }

    /// <summary>Ends the feed, from any thread: Next returns null from now on.</summary>
    public void Close() => _stream.Close();

    internal bool IsClosed => _stream.IsClosed;
}
