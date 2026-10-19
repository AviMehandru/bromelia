using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>What a step produced (stored as <c>job_steps.output</c>), for the steps after it, and the read errors it
/// saw, so a job resumed after it still ends with them (JobOutcome.decide).</summary>
public sealed record StepOutput(StepKind Kind, JsonValue Data, BroMessage? Summary = null, int ReadErrors = 0);
