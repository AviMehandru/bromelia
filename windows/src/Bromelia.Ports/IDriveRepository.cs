using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>drives and drive_stats_daily.</summary>
public interface IDriveRepository
{
    Task Upsert(DriveRecord drive);

    Task<IReadOnlyList<DriveRecord>> All();

    /// <summary>Adds stats to the day's counters (day: YYYY-MM-DD).</summary>
    Task RecordStats(string driveId, string day, DriveStats stats);
}
