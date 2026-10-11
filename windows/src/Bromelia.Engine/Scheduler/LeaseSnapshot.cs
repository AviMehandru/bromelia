using System.Collections.Generic;
using System.Linq;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>The data of a leaseChanged event (shared/schema/internal-events.json): who holds which drive (by drive
/// id), the acquisition slots held, and the jobs holding anything, per queue.</summary>
public sealed record LeaseSnapshot(IReadOnlyList<DriveHolder> Drives, int Acquisitions, JobCounts Jobs)
{
    public static readonly LeaseSnapshot Empty = new(new DriveHolder[0], 0, new JobCounts(0, 0, 0));

    public bool Equals(LeaseSnapshot? other) =>
        other is not null && Drives.SequenceEqual(other.Drives) && Acquisitions == other.Acquisitions && Jobs == other.Jobs;

    public override int GetHashCode() => Acquisitions;

    public static JsonValue Json(LeaseSnapshot snapshot) => JsonValue.Of(
        ("drives", JsonValue.Of(snapshot.Drives.Select(d => JsonValue.Of(("drive", JsonValue.Of(d.Drive)), ("job", JsonValue.Of(d.Job.Value)))))),
        ("acquisitions", JsonValue.Of(snapshot.Acquisitions)),
        ("jobs", JsonValue.Of(
            ("acquisition", JsonValue.Of(snapshot.Jobs.Acquisition)),
            ("processing", JsonValue.Of(snapshot.Jobs.Processing)),
            ("maintenance", JsonValue.Of(snapshot.Jobs.Maintenance)))));

    /// <summary>A leaseChanged event's data, typed; null when it isn't one.</summary>
    public static LeaseSnapshot? Parse(JsonValue json)
    {
        if (json["drives"]?.AsArray is not { } drives || json["acquisitions"]?.AsInteger is not { } acquisitions || json["jobs"] is not { } jobs)
            return null;
        var holders = new List<DriveHolder>();
        foreach (var d in drives)
        {
            if (d["drive"]?.AsString is not { } drive || d["job"]?.AsString is not { } job) return null;
            holders.Add(new DriveHolder(drive, new Id(job)));
        }
        if (jobs["acquisition"]?.AsInteger is not { } a || jobs["processing"]?.AsInteger is not { } p || jobs["maintenance"]?.AsInteger is not { } m)
            return null;
        return new LeaseSnapshot(holders, (int)acquisitions, new JobCounts((int)a, (int)p, (int)m));
    }
}
