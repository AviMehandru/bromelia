using System;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Engine;

/// <summary>The Clock port as engine code sees it: now, monotonic and local time as the clock says; a timer's handler
/// runs on the executor, whatever thread the clock fires it on, and never after the timer was cancelled.</summary>
public sealed class EngineClock
{
    readonly IClock _clock;
    readonly EngineExecutor _executor;

    public EngineClock(IClock clock, EngineExecutor executor)
    {
        _clock = clock;
        _executor = executor;
    }

    public Instant Now() => _clock.Now();

    public Duration Monotonic() => _clock.Monotonic();

    public LocalTime LocalTime(Instant instant) => _clock.LocalTime(instant);

    /// <summary>Call on the executor; <paramref name="handler"/> runs there.</summary>
    public EngineTimer Timer(TimerSchedule schedule, Action handler)
    {
        var timer = new EngineTimer(handler, schedule is TimerSchedule.At);
        timer.Handle = _clock.Timer(schedule, () => _executor.Post(timer.Fire));
        return timer;
    }
}
