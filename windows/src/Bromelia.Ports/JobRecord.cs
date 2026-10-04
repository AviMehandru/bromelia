using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>A row of jobs.</summary>
public sealed record JobRecord(
    Id Id,
    JobKind Kind,
    Id? ParentId,
    JobState State,
    Outcome? Outcome,
    BroError? Error,
    JsonValue? BlockedBy,
    JsonValue? Decision,
    Queue Queue,
    int Position,
    string? DriveId,
    long? MediaGeneration,
    bool Automatic,
    string? Mode,
    string Title,
    string? Fingerprint,
    JobRequest Request,
    JobPlan? Plan,
    Id? UnitId,
    Instant CreatedAt,
    Instant? StartedAt = null,
    Instant? FinishedAt = null);
