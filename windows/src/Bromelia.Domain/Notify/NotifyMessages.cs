using System.Collections.Generic;
using System.Linq;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>The text of notifications, as message codes.</summary>
public static class NotifyMessages
{
    /// <summary>"MKV finished: Inception", then what happened to the files: saved (also when a post-processing step
    /// failed), kept with read errors, or the job's error (else the outcome).</summary>
    public static TitleAndBody ForJob(JobSummary job, Outcome outcome)
    {
        var title = new BroMessage(MessageCode.NotifyTitleJob, Severity.Info, ("mode", JsonValue.Of(EnumWire.Name(job.Mode))),
            ("outcome", JsonValue.Of(EnumWire.Name(outcome))), ("what", JsonValue.Of(job.What)));
        BroMessage line = outcome switch
        {
            Outcome.Succeeded or Outcome.SucceededStepsFailed => new BroMessage(MessageCode.NotifyBodySaved, Severity.Info,
                ("count", JsonValue.Of(job.Files)), ("path", JsonValue.Of(job.Path))),
            Outcome.SucceededWithReadErrors => new BroMessage(MessageCode.NotifyBodyReadErrors, Severity.Info, ("path", JsonValue.Of(job.Path))),
            _ => new BroMessage(MessageCode.NotifyBodyError, Severity.Info, ("error", (job.Error ?? OutcomeMessage(outcome)).ToJson())),
        };
        return new TitleAndBody(title, new[] { line });
    }

    private static BroMessage OutcomeMessage(Outcome outcome) => new(outcome switch
    {
        Outcome.Cancelled => MessageCode.OutcomeCancelled,
        Outcome.Skipped => MessageCode.OutcomeSkipped,
        Outcome.Interrupted => MessageCode.OutcomeInterrupted,
        _ => MessageCode.OutcomeFailed,
    });

    /// <summary>"Archive check: all 12 folder(s) OK", or how many are damaged with a line for each of the first
    /// ten (a folder that isn't OK counts as damaged).</summary>
    public static TitleAndBody ForCheck(IReadOnlyList<FolderCheck> results)
    {
        var bad = results.Where(r => r.Result.Result != CheckResult.Ok).ToList();
        if (bad.Count == 0)
            return new TitleAndBody(new BroMessage(MessageCode.CheckNotifyAllOk, Severity.Info, ("total", JsonValue.Of(results.Count))),
                new[] { new BroMessage(MessageCode.CheckNotifyEveryFileMatches) });
        var lines = bad.Take(10).Select(r => new BroMessage(MessageCode.CheckNotifyFolder, Severity.Info, ("folder", JsonValue.Of(r.Folder)),
            ("summary", r.Result.Summary.ToJson()))).ToList();
        if (bad.Count > 10) lines.Add(new BroMessage(MessageCode.CheckNotifyMore, Severity.Info, ("count", JsonValue.Of(bad.Count - 10))));
        return new TitleAndBody(new BroMessage(MessageCode.CheckNotifyDamaged, Severity.Info, ("damaged", JsonValue.Of(bad.Count)),
            ("total", JsonValue.Of(results.Count))), lines);
    }
}
