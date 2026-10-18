using System.Collections.Generic;
using System.Linq;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>What PlanStep decided, for the steps after it: the mode, the profile (its id and every field, so a resumed
/// job keeps it), the library and whether it was offline, the titles to rip (numbered as in the listing Probe read; with
/// the tracks and file name chosen by hand), the minimum length of a one-pass rip, the bytes the titles need (before the
/// margin; null: no check) and whether they are reserved, the archived unit Decide asks about, and the identity under
/// the mode.</summary>
public sealed record PlanOutput(
    RipMode Mode,
    string ProfileId,
    JsonValue Profile,
    string LibraryId,
    bool LibraryOffline,
    IReadOnlyList<TitleChoice> Titles,
    int? OnePassMinLength,
    long? SpaceNeed,
    bool SpaceReserved,
    ArchivedMatch? AskArchived,
    Identity Identity)
{
    public static JsonValue Encode(PlanOutput plan) => JsonValue.Of(
        ("mode", JsonValue.Of(EnumWire.Name(plan.Mode))), ("profileId", JsonValue.Of(plan.ProfileId)), ("profile", plan.Profile),
        ("libraryId", JsonValue.Of(plan.LibraryId)), ("libraryOffline", JsonValue.Of(plan.LibraryOffline)),
        ("titles", JsonValue.Of(plan.Titles.Select(t =>
        {
            var members = new List<(string, JsonValue)> { ("index", JsonValue.Of(t.Index)) };
            if (t.Tracks is { } tracks) members.Add(("tracks", JsonValue.Of(tracks.Select(k => JsonValue.Of(k)))));
            if (t.Name.Length > 0) members.Add(("name", JsonValue.Of(t.Name)));
            return JsonValue.Of(members.ToArray());
        }))),
        ("onePass", plan.OnePassMinLength is { } m ? JsonValue.Of(m) : JsonValue.Null.Instance),
        ("spaceNeed", plan.SpaceNeed is { } n ? JsonValue.Of(n) : JsonValue.Null.Instance), ("spaceReserved", JsonValue.Of(plan.SpaceReserved)),
        ("askArchived", plan.AskArchived is { } a ? ArchivedMatch.Encode(a) : JsonValue.Null.Instance),
        ("identity", Identity.Encode(plan.Identity)));

    /// <summary>The plan in the job's Plan output; null when Plan hasn't run.</summary>
    public static PlanOutput? Of(JobSnapshot job) => JobSnapshot.Output(job, StepKind.Plan) is JsonValue.Object o ? Decode(o) : null;

    public static PlanOutput Decode(JsonValue json) => new(
        EnumWire.Parse<RipMode>(json["mode"]?.AsString) ?? RipMode.Mkv, json["profileId"]?.AsString ?? "", json["profile"] ?? JsonValue.Of(),
        json["libraryId"]?.AsString ?? "", json["libraryOffline"]?.AsBool ?? false,
        (json["titles"]?.AsArray ?? new List<JsonValue>()).Select(t => new TitleChoice((int)(t["index"]?.AsInteger ?? 0), true,
            t["tracks"]?.AsArray?.Select(k => (int)(k.AsInteger ?? 0)).ToList(), t["name"]?.AsString ?? "")).ToList(),
        (int?)json["onePass"]?.AsInteger, json["spaceNeed"]?.AsInteger, json["spaceReserved"]?.AsBool ?? false,
        ArchivedMatch.Decode(json["askArchived"]), Identity.Decode(json["identity"] ?? JsonValue.Of()));
}
