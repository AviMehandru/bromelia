using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Threading;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Adapters;

/// <summary>The real clock: wall time, a monotonic clock, cancellable sleeps and timers.</summary>
public sealed class SystemClock : IClock
{
    static readonly long Start = Stopwatch.GetTimestamp();

    public Instant Now() => new(DateTimeOffset.UtcNow.ToUnixTimeMilliseconds());

    public Duration Monotonic() => new((double)(Stopwatch.GetTimestamp() - Start) / Stopwatch.Frequency);

    public async Task Sleep(Duration duration, CancellationToken cancel)
    {
        try
        {
            await Task.Delay(TimeSpan.FromSeconds(Math.Max(0, duration.Seconds)), cancel.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.JobCancelled)));
        }
        if (cancel.IsCancelled) throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.JobCancelled)));
    }

    /// <summary>A timer lives until it has fired (At) or is cancelled, whether or not the handle is kept.</summary>
    public ITimerHandle Timer(TimerSchedule schedule, Action handler)
    {
        var (due, period) = schedule switch
        {
            TimerSchedule.At at => (TimeSpan.FromMilliseconds(Math.Max(0, at.Instant.UnixMilliseconds - Now().UnixMilliseconds)), Timeout.InfiniteTimeSpan),
            TimerSchedule.Every every => (TimeSpan.FromSeconds(every.Interval.Seconds), TimeSpan.FromSeconds(every.Interval.Seconds)),
            _ => throw new ArgumentOutOfRangeException(nameof(schedule)),
        };
        var handle = new Handle();
        lock (Live) Live.Add(handle);
        handle.Timer = new System.Threading.Timer(_ =>
        {
            handler();
            if (schedule is TimerSchedule.At) handle.Cancel();
        }, null, due, period);
        return handle;
    }

    /// <summary>The running timers: a System.Threading.Timer nobody refers to is collected.</summary>
    static readonly HashSet<Handle> Live = new();

    sealed class Handle : ITimerHandle
    {
        public System.Threading.Timer? Timer;

        public void Cancel()
        {
            lock (Live) Live.Remove(this);
            Timer?.Dispose();
        }
    }
}
