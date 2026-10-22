using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Engine.Tests;

/// <summary>
/// A clock that moves only when told (the scenarios' fake clock): it starts at 2026-10-02T20:00:00.000Z, and
/// <see cref="Advance"/> fires each due timer in due order (ties in the order they were made), with the time set to
/// its due time, then calls <see cref="Settle"/>. Its local time is UTC plus <see cref="UtcOffsetMinutes"/> (0 unless
/// set; no daylight saving time). Safe to use from any thread.
/// </summary>
internal sealed class ManualClock : IClock
{
    public const long StartMs = 1_790_971_200_000;

    readonly object _lock = new();
    readonly List<PendingTimer> _timers = new();
    long _nowMs = StartMs;
    long _made;

    /// <summary>The local time's fixed offset from UTC.</summary>
    public volatile int UtcOffsetMinutes;

    /// <summary>Runs after each fired timer (a test waits for the executor there).</summary>
    public Action? Settle;

    sealed class PendingTimer : ITimerHandle
    {
        public long DueMs;
        public long IntervalMs;
        public long Order;
        public Action Handler = () => { };
        public bool Cancelled;
        public void Cancel() => Cancelled = true;
    }

    public Instant Now()
    {
        lock (_lock) return new Instant(_nowMs);
    }

    public Duration Monotonic()
    {
        lock (_lock) return new Duration((_nowMs - StartMs) / 1000.0);
    }

    public Task Sleep(Duration duration, CancellationToken cancel) => throw new NotSupportedException("not used by the engine");

    public LocalTime LocalTime(Instant instant)
    {
        var t = DateTimeOffset.FromUnixTimeMilliseconds(instant.UnixMilliseconds).ToOffset(TimeSpan.FromMinutes(UtcOffsetMinutes));
        return new LocalTime(t.Year, t.Month, t.Day, t.Hour, t.Minute, t.Second);
    }

    public ITimerHandle Timer(TimerSchedule schedule, Action handler)
    {
        lock (_lock)
        {
            var t = new PendingTimer { Handler = handler, Order = _made++ };
            switch (schedule)
            {
                case TimerSchedule.At at:
                    t.DueMs = at.Instant.UnixMilliseconds;
                    break;
                case TimerSchedule.Every every:
                    t.IntervalMs = Math.Max(1, (long)Math.Round(every.Interval.Seconds * 1000));
                    t.DueMs = _nowMs + t.IntervalMs;
                    break;
            }
            _timers.Add(t);
            return t;
        }
    }

    public void Advance(long ms)
    {
        long target;
        lock (_lock) target = _nowMs + ms;
        while (true)
        {
            PendingTimer? next;
            lock (_lock)
            {
                _timers.RemoveAll(t => t.Cancelled);
                next = _timers.Where(t => t.DueMs <= target).OrderBy(t => t.DueMs).ThenBy(t => t.Order).FirstOrDefault();
                if (next == null)
                {
                    _nowMs = target;
                    return;
                }
                _nowMs = Math.Max(_nowMs, next.DueMs);
                if (next.IntervalMs > 0) next.DueMs += next.IntervalMs;
                else _timers.Remove(next);
            }
            next.Handler();
            Settle?.Invoke();
        }
    }
}
