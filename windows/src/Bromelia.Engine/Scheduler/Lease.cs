using System.Collections.Generic;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>What one acquire gave a step; numbered in grant order. <see cref="Scheduler.Release"/> gives it back, except its drives: they stay
/// with the job until <see cref="Scheduler.ReleaseDrives"/> or <see cref="Scheduler.Finish"/>.</summary>
public sealed class Lease
{
    internal readonly List<(string Key, int Count)> Counted;
    internal bool Released;

    internal Lease(Id jobId, long seq, List<(string, int)> counted)
    {
        JobId = jobId;
        Seq = seq;
        Counted = counted;
    }

    public Id JobId { get; }

    /// <summary>Leases are numbered in the order they were granted.</summary>
    public long Seq { get; }
}
