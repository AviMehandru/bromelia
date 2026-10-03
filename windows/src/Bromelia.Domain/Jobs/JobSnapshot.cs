using System.Collections.Generic;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>An immutable view of a job that a step gets in its context (plan §12.2): its request, its plan and
/// the outputs of the steps before it.</summary>
public sealed record JobSnapshot(Id Id, JobRequest Request, JobPlan? Plan, IReadOnlyList<StepOutput> Outputs);
