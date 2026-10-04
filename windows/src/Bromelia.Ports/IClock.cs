using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>Time.</summary>
public interface IClock
{
    Instant Now();

    /// <summary>Time since an arbitrary start; never goes back.</summary>
    Duration Monotonic();

    /// <summary>Fails with job.cancelled when cancelled first.</summary>
    Task Sleep(Duration duration, CancellationToken cancel);

    ITimerHandle Timer(TimerSchedule schedule, Action handler);
}
