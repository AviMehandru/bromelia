using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>A row of checks: one check of a unit, a replica or a folder.</summary>
public sealed record CheckRecord(
    Id Id,
    Id? UnitId,
    Id? ReplicaId,
    string Folder,
    Id? JobId,
    Instant StartedAt,
    Instant? FinishedAt,
    CheckResult Result,
    int Files,
    long Bytes,
    IReadOnlyList<string> Changed,
    IReadOnlyList<string> Unreadable,
    IReadOnlyList<string> Missing,
    IReadOnlyList<string> Unlisted,
    BroMessage? Error = null);
