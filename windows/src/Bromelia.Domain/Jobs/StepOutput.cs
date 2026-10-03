using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>What a step produced (stored as <c>job_steps.output</c>), for the steps after it.</summary>
public sealed record StepOutput(StepKind Kind, JsonValue Data, BroMessage? Summary = null);
