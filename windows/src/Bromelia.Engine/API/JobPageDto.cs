using System.Collections.Generic;
using System.Linq;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>A page of jobs (dtos.json JobPage): newest first; <see cref="Next"/> is the cursor of the next page, null on
/// the last; <see cref="Total"/> counts every job the query matches.</summary>
public sealed record JobPageDto(IReadOnlyList<JobDto> Items, string? Next, int Total)
{
    public static JsonValue Json(JobPageDto page) => JsonValue.Of(
        ("items", JsonValue.Of(page.Items.Select(JobDto.Json))),
        ("next", page.Next is { } next ? JsonValue.Of(next) : JsonValue.Null.Instance),
        ("total", JsonValue.Of(page.Total)));
}
