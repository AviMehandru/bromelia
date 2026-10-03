using System.Collections.Generic;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>The steps a job runs and what PlanStep decided (stored as <c>jobs.plan</c>): the mode, profile and
/// library, plus the rest in <see cref="Details"/>.</summary>
public sealed record JobPlan(
    IReadOnlyList<StepKind> Steps,
    string? Mode = null,
    string? ProfileId = null,
    string? LibraryId = null,
    JsonValue? Details = null);
