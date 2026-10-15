using System.Collections.Generic;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Engine;

/// <summary>A job, as clients see it (dtos.json Job). Optional members are left out of the JSON when absent.</summary>
public sealed record JobDto(
    Id Id,
    JobKind Kind,
    JobState State,
    Outcome? Outcome,
    Queue Queue,
    int? Position,
    Id? ParentId,
    string? DriveId,
    Id? SessionId,
    string Title,
    string? Mode,
    bool Automatic,
    Instant CreatedAt,
    Instant? StartedAt,
    Instant? FinishedAt,
    ProgressDto? Progress,
    BroMessage? BlockedBy,
    DecisionDto? Decision,
    BroError? Error,
    Id? UnitId,
    string? OutputPath,
    string? Fingerprint,
    JobTotals? Counts)
{
    /// <summary>The stored job as clients see it, without what only a running job has (progress, blockedBy, decision,
    /// position, counts).</summary>
    public static JobDto Of(JobRecord record) => new(record.Id, record.Kind, record.State, record.Outcome, record.Queue, null, record.ParentId,
        record.DriveId, record.Request.SessionId, record.Title, record.Mode, record.Automatic, record.CreatedAt, record.StartedAt, record.FinishedAt,
        null, null, null, record.Error, record.UnitId, null, record.Fingerprint, null);

    public static JsonValue Json(JobDto job)
    {
        var m = new List<KeyValuePair<string, JsonValue>>();
        void Add(string key, JsonValue? value)
        {
            if (value != null) m.Add(new(key, value));
        }
        Add("id", JsonValue.Of(job.Id.Value));
        Add("kind", JsonValue.Of(EnumWire.Name(job.Kind)));
        Add("state", JsonValue.Of(EnumWire.Name(job.State)));
        Add("outcome", job.Outcome is { } o ? JsonValue.Of(EnumWire.Name(o)) : null);
        Add("queue", JsonValue.Of(EnumWire.Name(job.Queue)));
        Add("position", job.Position is { } p ? JsonValue.Of(p) : null);
        Add("parentId", job.ParentId is { } parent ? JsonValue.Of(parent.Value) : null);
        Add("driveId", job.DriveId is { } d ? JsonValue.Of(d) : null);
        Add("sessionId", job.SessionId is { } s ? JsonValue.Of(s.Value) : null);
        Add("title", JsonValue.Of(job.Title));
        Add("mode", job.Mode is { } mode ? JsonValue.Of(mode) : null);
        Add("automatic", JsonValue.Of(job.Automatic));
        Add("createdAt", JsonValue.Of(Instant.Format(job.CreatedAt)));
        Add("startedAt", job.StartedAt is { } started ? JsonValue.Of(Instant.Format(started)) : null);
        Add("finishedAt", job.FinishedAt is { } finished ? JsonValue.Of(Instant.Format(finished)) : null);
        Add("progress", job.Progress is { } progress ? ProgressDto.Json(progress) : null);
        Add("blockedBy", job.BlockedBy?.ToJson());
        Add("decision", job.Decision is { } decision ? DecisionDto.Json(decision) : null);
        Add("error", job.Error is { } error ? JsonDto.Error(error) : null);
        Add("unitId", job.UnitId is { } unit ? JsonValue.Of(unit.Value) : null);
        Add("outputPath", job.OutputPath is { } path ? JsonValue.Of(path) : null);
        Add("fingerprint", job.Fingerprint is { } fp ? JsonValue.Of(fp) : null);
        Add("counts", job.Counts is { } c
            ? JsonValue.Of(("files", JsonValue.Of(c.Files)), ("warnings", JsonValue.Of(c.Warnings)), ("errors", JsonValue.Of(c.Errors)),
                ("readErrors", JsonValue.Of(c.ReadErrors)))
            : null);
        return new JsonValue.Object(m);
    }
}

/// <summary>Helpers the DTOs share.</summary>
internal static class JsonDto
{
    /// <summary>A whole number as an integer, so 1.0 is written 1 on every platform.</summary>
    public static JsonValue Number(double d) =>
        d == System.Math.Floor(d) && System.Math.Abs(d) < 9e15 ? JsonValue.Of((long)d) : new JsonValue.Number(d);

    /// <summary>{code, params, cause} (shared/schema/common.json BroError), params left out when empty.</summary>
    public static JsonValue Error(BroError error)
    {
        var m = new List<KeyValuePair<string, JsonValue>> { new("code", JsonValue.Of(error.Code)) };
        if (error.Params.Members.Count > 0) m.Add(new("params", error.Params));
        if (error.Cause is { } cause) m.Add(new("cause", Error(cause)));
        return new JsonValue.Object(m);
    }
}
