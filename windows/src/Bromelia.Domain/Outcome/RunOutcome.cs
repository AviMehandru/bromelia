using System.Collections.Generic;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>What one makemkvcon run amounted to. makemkvcon exits 0 when a title fails, so the saved / failed
/// counts (5036, 5037, 5004) and the files the run produced decide, not the exit status alone.</summary>
public sealed record RunOutcome(
    int? Saved,
    int? Failed,
    int ExitCode,
    RobotMessage? FirstError,
    RobotMessage? SpaceWarning,
    IReadOnlyList<RobotMessage> ReadErrors,
    BroMessage? DriveMismatch,
    string? DebugLog,
    StatusWord Status,
    BroMessage? Error)
{
    /// <summary>Cancelled when the run was cancelled. Failed when MakeMKV's space warning or a renumbered drive
    /// stopped it, when it stalled, exited non-zero, reported a failed title or produced nothing; the error is the
    /// reason (the first error MakeMKV reported, else the last, else the exit status). Errors when it read the
    /// disc with read errors (rip.readErrors). Success otherwise.</summary>
    public static RunOutcome Classify(RunAccumulator accumulator, ProcessExit exit, int producedFiles)
    {
        var a = accumulator;
        RunOutcome With(StatusWord status, BroMessage? error) =>
            new(a.Saved, a.Failed, exit.Status, a.FirstError, a.SpaceWarning, a.ReadErrors, a.DriveMismatch, a.DebugLog, status, error);

        // These stop makemkvcon themselves, so they come before cancellation.
        if (a.DriveMismatch is { } mismatch) return With(StatusWord.Failed, mismatch);
        if (a.SpaceWarning is { } space)
            return With(StatusWord.Failed, new BroMessage(MessageCode.SpaceMakemkvWarning, Severity.Error, ("text", JsonValue.Of(space.Text))));
        if (exit.Stalled is { } silence)
            return With(StatusWord.Failed, new BroMessage(MessageCode.MakemkvStalled, Severity.Error,
                ("minutes", JsonValue.Of((long)(silence.Seconds / 60))), ("abandoned", JsonValue.Of(exit.Abandoned))));
        if (exit.Cancelled || exit.Abandoned) return With(StatusWord.Cancelled, null);
        if (exit.Status != 0 || (a.Failed ?? 0) > 0 || producedFiles == 0)
        {
            var reason = a.FirstError ?? (a.Errors.Count > 0 ? a.Errors[a.Errors.Count - 1] : null);
            return With(StatusWord.Failed, reason is { } r
                ? new BroMessage(MessageCode.MakemkvMessage, Severity.Error, ("text", JsonValue.Of(r.Text)))
                : new BroMessage(MessageCode.ProcessExitStatus, Severity.Error, ("status", JsonValue.Of(exit.Status))));
        }
        if (a.ReadErrors.Count > 0)
            return With(StatusWord.Errors, new BroMessage(MessageCode.RipReadErrors, Severity.Error, ("count", JsonValue.Of(a.ReadErrors.Count))));
        return With(StatusWord.Success, null);
    }
}
