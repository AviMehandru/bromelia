using System.Collections.Generic;
using Bromelia.Domain;

namespace Bromelia.Engine;

/// <summary>The steps a job starts with (plan §15.2, §20.1); the Plan step appends the rest.
/// shared/fixtures/engine/job-planner.cases.json.</summary>
public static class JobPlanner
{
    /// <summary>A video disc: awaitMedia when the rip is automatic and from a drive, probe, reconcile when a session
    /// made the choices, identify, plan. Kinds whose steps aren't built yet get none.</summary>
    public static IReadOnlyList<StepKind> Initial(JobRequest request)
    {
        var steps = new List<StepKind>();
        if (request.Kind != JobKind.VideoDisc) return steps;
        if (request.Automatic && request.DriveId != null) steps.Add(StepKind.AwaitMedia);
        steps.Add(StepKind.Probe);
        if (request.SessionId != null) steps.Add(StepKind.Reconcile);
        steps.Add(StepKind.Identify);
        steps.Add(StepKind.Plan);
        return steps;
    }
}
