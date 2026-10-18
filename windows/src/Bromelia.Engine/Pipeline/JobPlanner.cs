using System.Collections.Generic;
using System.Linq;
using Bromelia.Domain;
using Bromelia.Ports;

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

    /// <summary>The queue a job of the kind starts in (plan §21): a job that reads a disc, acquisition; checks and copies of
    /// the archive, maintenance; commands and transcodes, processing.</summary>
    public static Queue Queue(JobKind kind) => kind switch
    {
        JobKind.VideoDisc or JobKind.AudioCd or JobKind.DataDisc or JobKind.ReadErrorRetry => Domain.Queue.Acquisition,
        JobKind.RunCommand or JobKind.Transcode => Domain.Queue.Processing,
        _ => Domain.Queue.Maintenance,
    };

    /// <summary>The steps a run doesn't do again (plan §12.3): the leading ones, by seq, that succeeded or were skipped,
    /// or failed after the commit (JobOutcome.afterCommit: the runner went on after them).</summary>
    public static IReadOnlyList<StepRecord> Done(IReadOnlyList<StepRecord> steps)
    {
        var done = new List<StepRecord>();
        foreach (var s in steps.OrderBy(s => s.Seq))
        {
            var finished = s.State is StepState.Succeeded or StepState.Skipped || (s.State == StepState.Failed && JobOutcome.AfterCommit(s.Kind));
            if (s.Seq != done.Count || !finished) break;
            done.Add(s);
        }
        return done;
    }
}
