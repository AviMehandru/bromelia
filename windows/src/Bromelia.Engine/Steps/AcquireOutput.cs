using System.Collections.Generic;
using System.Linq;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>What AcquireStep left in the job's staging folder, by name (plan §12.4; the steps after it use these lists,
/// never a count): the titles in plan order, the files of a run no title owns (a file an "all" run made that the
/// listing doesn't name, and what a failed run left), the backup (a folder or an image) when it is kept, disc-info.json
/// when it was written, and the read errors of runs that ripped several titles at once or made the backup.</summary>
public sealed record AcquireOutput(
    IReadOnlyList<AcquiredTitle> Titles,
    IReadOnlyList<string> Others,
    string? Backup,
    string? DiscInfo,
    IReadOnlyList<string> ReadErrors)
{
    public static JsonValue Encode(AcquireOutput output) => JsonValue.Of(
        ("titles", JsonValue.Of(output.Titles.Select(AcquiredTitle.Encode))), ("others", JsonValue.Of(output.Others.Select(f => JsonValue.Of(f)))),
        ("backup", JsonValue.Of(output.Backup)), ("discInfo", JsonValue.Of(output.DiscInfo)),
        ("readErrors", JsonValue.Of(output.ReadErrors.Select(e => JsonValue.Of(e)))));

    public static AcquireOutput Decode(JsonValue json) => new(
        (json["titles"]?.AsArray ?? new List<JsonValue>()).Select(AcquiredTitle.Decode).ToList(),
        (json["others"]?.AsArray ?? new List<JsonValue>()).Select(f => f.AsString ?? "").ToList(),
        json["backup"]?.AsString, json["discInfo"]?.AsString,
        (json["readErrors"]?.AsArray ?? new List<JsonValue>()).Select(e => e.AsString ?? "").ToList());

    /// <summary>The job's Acquire output; null when Acquire hasn't run.</summary>
    public static AcquireOutput? Of(JobSnapshot job) => JobSnapshot.Output(job, StepKind.Acquire) is JsonValue.Object o ? Decode(o) : null;

    /// <summary>Every name Acquire made in staging: the titles' files, the others, the backup and disc-info.json.</summary>
    public static IReadOnlyList<string> Produced(AcquireOutput output) =>
        output.Titles.SelectMany(t => t.Files).Concat(output.Others).Concat(new[] { output.Backup, output.DiscInfo }.OfType<string>()).ToList();

    /// <summary>How many read errors MakeMKV reported in all.</summary>
    public static int ReadErrorCount(AcquireOutput output) => output.Titles.Sum(t => t.ReadErrors.Count) + output.ReadErrors.Count;
}
