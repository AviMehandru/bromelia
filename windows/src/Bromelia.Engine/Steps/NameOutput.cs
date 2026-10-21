using System.Collections.Generic;
using System.Linq;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>What NameStep decided (session F fills it; Seal reads it): the unit's folder relative to the library root
/// (Layouts.paths, ConflictNamer), its name (a quarantined unit goes to _Quarantine/&lt;name&gt; [&lt;tag&gt;]), whether
/// the folder is shared (a media server layout: the commit merges item by item), and every produced item by the name
/// Name gave it in staging. Name renames inside staging, so staging is laid out as the unit's folder will be.</summary>
public sealed record NameOutput(string Folder, string Name, bool Merge, IReadOnlyList<NamedFile> Files)
{
    public static JsonValue Encode(NameOutput output) => JsonValue.Of(("folder", JsonValue.Of(output.Folder)),
        ("name", JsonValue.Of(output.Name)), ("merge", JsonValue.Of(output.Merge)),
        ("files", JsonValue.Of(output.Files.Select(NamedFile.Encode))));

    public static NameOutput Decode(JsonValue json) => new(json["folder"]?.AsString ?? "", json["name"]?.AsString ?? "",
        json["merge"]?.AsBool ?? false, (json["files"]?.AsArray ?? new List<JsonValue>()).Select(NamedFile.Decode).ToList());

    /// <summary>The job's Name output; null when Name hasn't run.</summary>
    public static NameOutput? Of(JobSnapshot job) => JobSnapshot.Output(job, StepKind.Name) is JsonValue.Object o ? Decode(o) : null;

    /// <summary>The produced items as Acquire left them, when no Name step ran: each title's files (role title), the
    /// files no title owns (no role), the backup (role backup) and disc-info.json (role discInfo).</summary>
    public static IReadOnlyList<NamedFile> FromAcquire(AcquireOutput acquired) =>
        acquired.Titles.SelectMany(t => t.Files.Select(f => new NamedFile(f, "title", t.Index)))
            .Concat(acquired.Others.Select(f => new NamedFile(f, null, null)))
            .Concat(acquired.Backup is { } b ? new[] { new NamedFile(b, "backup", null) } : new NamedFile[0])
            .Concat(acquired.DiscInfo is { } d ? new[] { new NamedFile(d, "discInfo", null) } : new NamedFile[0]).ToList();
}
