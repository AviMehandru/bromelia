using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>Which jobs (GET /v1/jobs): in any of the states, with any of the outcomes (an empty list is any), of a
/// kind, in a queue, on a drive, for a unit, finished after an instant; newest first, a page at a time.</summary>
public sealed record JobQuery(
    IReadOnlyList<JobState> States,
    IReadOnlyList<Outcome> Outcomes,
    JobKind? Kind,
    Queue? Queue,
    string? DriveId,
    Id? UnitId,
    Instant? FinishedAfter,
    int Limit,
    int Offset);
