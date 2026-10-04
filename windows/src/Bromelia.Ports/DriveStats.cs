using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>A day's counters of drive_stats_daily.</summary>
public sealed record DriveStats(
    int Jobs,
    int FailedJobs,
    int ReadErrorJobs,
    int ReadErrors,
    long BytesRead,
    double SecondsReading);
