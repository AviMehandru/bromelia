using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>When a timer fires: once at an instant, or every interval.</summary>
public abstract record TimerSchedule
{
    public sealed record At(Instant Instant) : TimerSchedule;
    public sealed record Every(Duration Interval) : TimerSchedule;
}
