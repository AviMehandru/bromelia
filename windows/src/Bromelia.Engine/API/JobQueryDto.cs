using System.Collections.Generic;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>Which jobs a client asks for (dtos.json JobQuery, the query string of GET /v1/jobs): any of the states, any
/// of the outcomes (empty: any), a kind, a queue, a drive, a unit; a page of at most <see cref="Limit"/> (1–500) from
/// <see cref="Cursor"/> (the last page's next).</summary>
public sealed record JobQueryDto(
    IReadOnlyList<JobState> States,
    IReadOnlyList<Outcome> Outcomes,
    JobKind? Kind = null,
    Queue? Queue = null,
    string? DriveId = null,
    Id? UnitId = null,
    string? Cursor = null,
    int Limit = JobQueryDto.DefaultLimit)
{
    public const int DefaultLimit = 50;
    public const int MaxLimit = 500;
}
