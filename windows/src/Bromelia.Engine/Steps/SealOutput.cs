using System.Collections.Generic;
using System.Linq;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Engine;

/// <summary>What SealStep wrote into the job's staging folder, for Commit: the unit's id, the record's status (success,
/// or errors after read errors), the files SHA256SUMS lists (path relative to the unit's folder, size and SHA-256 as read
/// back from disk, role, title: unit_files rows), the record's file name, the logs copied, the note (READ ERRORS.txt) and
/// the quarantine tag Commit uses ("READ ERRORS"; null: a finished archive, committed into the library).</summary>
public sealed record SealOutput(
    Id UnitId,
    string Status,
    IReadOnlyList<UnitFile> Files,
    string Record,
    IReadOnlyList<string> Logs,
    string? Note,
    string? Tag)
{
    public static JsonValue Encode(SealOutput output) => JsonValue.Of(("unitId", JsonValue.Of(output.UnitId.Value)),
        ("status", JsonValue.Of(output.Status)),
        ("files", JsonValue.Of(output.Files.Select(f =>
        {
            var members = new List<(string, JsonValue)> { ("path", JsonValue.Of(f.Path)), ("size", JsonValue.Of(f.Size)), ("sha256", JsonValue.Of(f.Sha256)) };
            if (f.Role.Length > 0) members.Add(("role", JsonValue.Of(f.Role)));
            if (f.Title is { } t) members.Add(("title", JsonValue.Of(t)));
            return JsonValue.Of(members.ToArray());
        }))),
        ("record", JsonValue.Of(output.Record)), ("logs", JsonValue.Of(output.Logs.Select(l => JsonValue.Of(l)))),
        ("note", JsonValue.Of(output.Note)), ("tag", JsonValue.Of(output.Tag)));

    public static SealOutput Decode(JsonValue json) => new(new Id(json["unitId"]?.AsString ?? ""), json["status"]?.AsString ?? "",
        (json["files"]?.AsArray ?? new List<JsonValue>()).Select(f => new UnitFile(f["path"]?.AsString ?? "", f["size"]?.AsInteger ?? 0,
            f["sha256"]?.AsString ?? "", f["role"]?.AsString ?? "", (int?)f["title"]?.AsInteger)).ToList(),
        json["record"]?.AsString ?? "", (json["logs"]?.AsArray ?? new List<JsonValue>()).Select(l => l.AsString ?? "").ToList(),
        json["note"]?.AsString, json["tag"]?.AsString);

    /// <summary>The job's Seal output; null when Seal hasn't run.</summary>
    public static SealOutput? Of(JobSnapshot job) => JobSnapshot.Output(job, StepKind.Seal) is JsonValue.Object o ? Decode(o) : null;
}
