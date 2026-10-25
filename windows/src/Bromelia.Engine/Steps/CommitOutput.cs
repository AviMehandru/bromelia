using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>What CommitStep did: the unit, its library, its folder relative to the library's root as committed (a free
/// name in _Quarantine when quarantined), whether it was merged into a folder that is shared, and whether it was
/// quarantined (plan §15.4: the unit's id in the job's output).</summary>
public sealed record CommitOutput(Id UnitId, string Library, string Path, bool Merged, bool Quarantined)
{
    public static JsonValue Encode(CommitOutput output) => JsonValue.Of(("unitId", JsonValue.Of(output.UnitId.Value)),
        ("library", JsonValue.Of(output.Library)), ("path", JsonValue.Of(output.Path)), ("merged", JsonValue.Of(output.Merged)),
        ("quarantined", JsonValue.Of(output.Quarantined)));

    public static CommitOutput Decode(JsonValue json) => new(new Id(json["unitId"]?.AsString ?? ""), json["library"]?.AsString ?? "",
        json["path"]?.AsString ?? "", json["merged"]?.AsBool ?? false, json["quarantined"]?.AsBool ?? false);

    /// <summary>The job's Commit output; null when Commit hasn't run.</summary>
    public static CommitOutput? Of(JobSnapshot job) => JobSnapshot.Output(job, StepKind.Commit) is JsonValue.Object o ? Decode(o) : null;
}
