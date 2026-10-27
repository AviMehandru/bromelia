using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>One step of a job's plan as JobDetail lists it (dtos.json JobDetail.steps): its state, times, error and
/// summary from the step's stored row; pending, with nothing else, when it has none.</summary>
public sealed record JobStepDto(int Seq, StepKind Kind, StepState State, Instant? StartedAt, Instant? FinishedAt, BroError? Error, BroMessage? Summary);
