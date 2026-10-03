using System.Collections.Generic;
using System.Linq;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>How a job ended, from its steps' results (plan §20.3): one pure function instead of today's chain of
/// flags in three run() methods.</summary>
public static class JobOutcome
{
    /// <summary>Steps after the archive is committed: their failures never fail a job.</summary>
    private static readonly HashSet<StepKind> AfterCommit = new()
    {
        StepKind.Publish, StepKind.Protect, StepKind.PostProcess, StepKind.Notify, StepKind.RunCommand, StepKind.Transcode,
    };

    /// <summary>In order: cancelled (job.cancelledByUser); interrupted (recovery.interrupted); skipped (the skip
    /// reason); a failed step that isn't after the commit fails the job with its error, or with the MakeMKV
    /// problem that explains it (the error as its cause); read errors give succeededWithReadErrors
    /// (rip.readErrors); a failed post-commit step that affects the outcome gives succeededStepsFailed; else
    /// succeeded.</summary>
    public static Decided Decide(IReadOnlyList<StepResult> stepResults, bool cancelled, OutcomePolicy policy)
    {
        if (cancelled) return new(Outcome.Cancelled, new BroMessage(MessageCode.JobCancelledByUser, Severity.Warning).ToError());
        if (stepResults.FirstOrDefault(s => s.State == StepState.Interrupted) is { } interrupted)
            return new(Outcome.Interrupted, new BroMessage(MessageCode.RecoveryInterrupted, Severity.Warning,
                ("step", JsonValue.Of(EnumWire.Name(interrupted.Kind)))).ToError());
        if (stepResults.FirstOrDefault(s => s.Skip != null) is { Skip: { } skip }) return new(Outcome.Skipped, skip.ToError());
        if (stepResults.FirstOrDefault(s => s.State == StepState.Failed && !AfterCommit.Contains(s.Kind)) is { } failed)
        {
            var error = failed.Error ?? new BroMessage(MessageCode.InternalUnexpected, Severity.Error).ToError();
            if (failed.Notice is { } notice && ProblemCode(notice) is { } problem)
                error = new BroMessage(problem, Severity.Error).ToError(error);
            return new(Outcome.Failed, error);
        }
        int readErrors = stepResults.Sum(s => s.ReadErrors);
        if (readErrors > 0)
            return new(Outcome.SucceededWithReadErrors, new BroMessage(MessageCode.RipReadErrors, Severity.Error, ("count", JsonValue.Of(readErrors))).ToError());
        if (stepResults.FirstOrDefault(s => s.State == StepState.Failed && s.AffectsOutcome) is { } post)
            return new(Outcome.SucceededStepsFailed, post.Error);
        return new(Outcome.Succeeded, null);
    }

    private static MessageCode? ProblemCode(MakemkvNotice notice) => notice switch
    {
        MakemkvNotice.KeyExpired => MessageCode.MakemkvKeyExpired,
        MakemkvNotice.EvaluationNotStarted => MessageCode.MakemkvEvaluationNotStarted,
        MakemkvNotice.VersionTooOld => MessageCode.MakemkvTooOld,
        MakemkvNotice.LibreDriveRequired => MessageCode.MakemkvLibreDriveRequired,
        _ => null,
    };
}
