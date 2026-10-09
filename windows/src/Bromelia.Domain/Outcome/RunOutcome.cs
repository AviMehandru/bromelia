using System;
using System.Collections.Generic;
using System.Linq;
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
    BroMessage? Error,
    IReadOnlyList<string> Produced)
{
    /// <summary>Cancelled when the run was cancelled. Failed when MakeMKV's space warning or a renumbered drive
    /// stopped it, when it stalled, exited non-zero, reported a failed title or produced nothing; the error is the
    /// reason (the first error MakeMKV reported, else the last, else the exit status, else process.savedNothing).
    /// A rip (<see cref="RunProduct.Titles"/>) also fails when MakeMKV didn't say how many titles it saved
    /// (makemkv.noSummary) or when that number isn't the number of new MKV files (makemkv.savedMismatch). Errors
    /// when it read the disc with read errors (rip.readErrors). Success otherwise.
    /// <paramref name="newNames"/> are the names in the destination that weren't there before the run; only
    /// <see cref="Products"/> of them count, and they become <see cref="Produced"/>.</summary>
    public static RunOutcome Classify(RunAccumulator accumulator, ProcessExit exit, RunProduct product, IReadOnlyList<string> newNames)
    {
        var a = accumulator;
        var produced = Products(product, newNames);
        RunOutcome With(StatusWord status, BroMessage? error) =>
            new(a.Saved, a.Failed, exit.Status, a.FirstError, a.SpaceWarning, a.ReadErrors, a.DriveMismatch, a.DebugLog, status, error, produced);

        // These stop makemkvcon themselves, so they come before cancellation.
        if (a.DriveMismatch is { } mismatch) return With(StatusWord.Failed, mismatch);
        if (a.SpaceWarning is { } space)
            return With(StatusWord.Failed, new BroMessage(MessageCode.SpaceMakemkvWarning, Severity.Error, ("text", JsonValue.Of(space.Text))));
        if (exit.Stalled is { } silence)
            return With(StatusWord.Failed, new BroMessage(MessageCode.MakemkvStalled, Severity.Error,
                ("minutes", JsonValue.Of((long)(silence.Seconds / 60))), ("abandoned", JsonValue.Of(exit.Abandoned))));
        if (exit.Cancelled || exit.Abandoned) return With(StatusWord.Cancelled, null);
        if (exit.Status != 0 || (a.Failed ?? 0) > 0 || (product != RunProduct.Nothing && produced.Count == 0))
        {
            var reason = a.FirstError ?? (a.Errors.Count > 0 ? a.Errors[a.Errors.Count - 1] : null);
            return With(StatusWord.Failed, reason is { } r
                ? new BroMessage(MessageCode.MakemkvMessage, Severity.Error, ("text", JsonValue.Of(r.Text)))
                : exit.Status != 0
                    ? new BroMessage(MessageCode.ProcessExitStatus, Severity.Error, ("status", JsonValue.Of(exit.Status)))
                    : new BroMessage(MessageCode.ProcessSavedNothing, Severity.Error, ("tool", JsonValue.Of("makemkvcon"))));
        }
        if (product == RunProduct.Titles)
        {
            if (a.Saved is not { } saved) return With(StatusWord.Failed, new BroMessage(MessageCode.MakemkvNoSummary, Severity.Error));
            if (saved != produced.Count)
                return With(StatusWord.Failed, new BroMessage(MessageCode.MakemkvSavedMismatch, Severity.Error, ("saved", JsonValue.Of(saved)),
                    ("produced", JsonValue.Of(produced.Count))));
        }
        if (a.ReadErrors.Count > 0)
            return With(StatusWord.Errors, new BroMessage(MessageCode.RipReadErrors, Severity.Error, ("count", JsonValue.Of(a.ReadErrors.Count))));
        return With(StatusWord.Success, null);
    }

    /// <summary>The new names a run of this kind produced, in the order given: MKV files for a rip (a name ending in
    /// .mkv, in any case, that isn't hidden), the disc structure for a backup (a BDMV, VIDEO_TS or HVDVD_TS folder, or
    /// an .iso image), the file itself for an image (any name that isn't hidden), nothing for a listing. Anything else
    /// (.DS_Store, Thumbs.db, a partial file) doesn't count.</summary>
    public static IReadOnlyList<string> Products(RunProduct product, IReadOnlyList<string> newNames) => product switch
    {
        RunProduct.Titles => newNames.Where(n => !n.StartsWith('.') && n.EndsWith(".mkv", StringComparison.OrdinalIgnoreCase)).ToList(),
        RunProduct.Backup => newNames.Where(n => !n.StartsWith('.') && (DiscFolders.Contains(n.ToUpperInvariant())
            || n.EndsWith(".iso", StringComparison.OrdinalIgnoreCase))).ToList(),
        RunProduct.Image => newNames.Where(n => !n.StartsWith('.')).ToList(),
        _ => Array.Empty<string>(),
    };

    static readonly HashSet<string> DiscFolders = new(StringComparer.Ordinal) { "BDMV", "VIDEO_TS", "HVDVD_TS" };
}
