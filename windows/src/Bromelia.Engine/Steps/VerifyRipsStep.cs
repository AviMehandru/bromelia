using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>Step 10 (plan §20.1; today's verify): checks what Acquire made before anything is named, hashed or moved. A
/// kept backup first (BackupCheck: backup.checkFailed). Then each title's MKV files, in plan order, against Probe's
/// listing title of the same index (MkvToolNix.probe, then RipCheck.check; ripcheck.checking): its notes as
/// ripcheck.note warnings, then ripcheck.checked and, with fewer tracks than the disc, ripcheck.trackSubset; a file
/// mkvmerge can't read (ripcheck.unreadable) or that doesn't match (ripcheck.mismatch) is rip.checkFailed, every title
/// is still checked, and the step fails with rip.failed or rip.titlesFailed. Without mkvmerge: ripcheck.cannotCheck and
/// no MKV is checked, as today. Skipped when Acquire left nothing. Data: {checked: [the names that passed]}.</summary>
public sealed class VerifyRipsStep : IStep
{
    public StepKind Kind => StepKind.VerifyRips;

    public Resumability Resumability => Resumability.Idempotent;

    public ResourceSet Resources(JobSnapshot job) => ResourceSet.None;

    public async Task<StepReport> Run(StepContext context)
    {
        var plan = PlanOutput.Of(context.Job);
        if (AcquireOutput.Of(context.Job) is not { } acquired || plan == null) return StepReport.Skip;
        var staging = context.Library.StagingDirectory(plan.LibraryId, context.Job.Id);
        var listing = ProbeOutput.Decode(JobSnapshot.Output(context.Job, StepKind.Probe) ?? JsonValue.Null.Instance).Listing;
        var passed = new List<string>();
        if (acquired.Backup is { } backup)
        {
            if (BackupCheck.Problem(context.Fs, staging + "/" + backup) is { } problem)
                return Failed(passed, new BroMessage(MessageCode.BackupCheckFailed, Severity.Error, ("problem", problem.ToJson())));
            passed.Add(backup);
        }
        var files = acquired.Titles.SelectMany(t => t.Files.Select(f => (t.Index, File: f))).ToList();
        var failures = new List<(int Title, BroMessage Problem)>();
        for (int i = 0; i < files.Count; i++)
        {
            var (index, file) = files[i];
            context.Progress.Report((double)i / files.Count, new BroMessage(MessageCode.RipcheckChecking, Severity.Info, ("file", JsonValue.Of(file))));
            MkvProbe? probe;
            try
            {
                probe = await context.Tools.Matroska.Probe(staging + "/" + file, context.Cancel);
            }
            catch (BroFailure f) when (f.Error.Code is "tool.missing" or "tool.notFoundAt")
            {
                context.Log.Log(new BroMessage(MessageCode.RipcheckCannotCheck, Severity.Warning));
                return Done(passed);
            }
            BroMessage? failure = null;
            var title = listing?.Titles.FirstOrDefault(t => t.Index == index);
            if (probe == null) failure = new BroMessage(MessageCode.RipcheckUnreadable, Severity.Info, ("file", JsonValue.Of(file)));
            else if (title != null)
            {
                var result = RipCheck.Check(probe, title);
                foreach (var note in result.Notes)
                    context.Log.Log(new BroMessage(MessageCode.RipcheckNote, Severity.Warning, ("file", JsonValue.Of(file)), ("note", note.ToJson())));
                if (result.Problems.Count > 0)
                    failure = new BroMessage(MessageCode.RipcheckMismatch, Severity.Info, ("file", JsonValue.Of(file)),
                        ("problems", JsonValue.Of(result.Problems.Select(p => p.ToJson()))));
            }
            if (failure != null)
            {
                context.Log.Log(new BroMessage(MessageCode.RipCheckFailed, Severity.Error, ("title", JsonValue.Of(index)), ("problem", failure.ToJson())));
                failures.Add((index, failure));
                continue;
            }
            context.Log.Log(new BroMessage(MessageCode.RipcheckChecked, Severity.Info, ("file", JsonValue.Of(file)),
                ("duration", JsonValue.Of((long)Math.Round(probe!.DurationSeconds ?? 0))), ("tracks", JsonValue.Of(probe.Tracks.Count)),
                ("chapters", JsonValue.Of(probe.ChapterCount))));
            if (title != null && title.Tracks.Count > 0 && probe.Tracks.Count < title.Tracks.Count)
                context.Log.Log(new BroMessage(MessageCode.RipcheckTrackSubset, Severity.Info, ("file", JsonValue.Of(file)),
                    ("have", JsonValue.Of(probe.Tracks.Count)), ("total", JsonValue.Of(title.Tracks.Count))));
            passed.Add(file);
        }
        if (failures.Count == 0) return Done(passed);
        JsonValue Reason((int Title, BroMessage Problem) f) => new BroMessage(MessageCode.RipTitleReason, Severity.Info,
            ("title", JsonValue.Of(f.Title.ToString(CultureInfo.InvariantCulture))), ("reason", f.Problem.ToJson())).ToJson();
        return Failed(passed, failures.Count == 1
            ? new BroMessage(MessageCode.RipFailed, Severity.Error, ("reason", Reason(failures[0])))
            : new BroMessage(MessageCode.RipTitlesFailed, Severity.Error, ("count", JsonValue.Of(failures.Count)),
                ("reasons", JsonValue.Of(failures.Select(Reason)))));
    }

    static JsonValue Data(List<string> passed) => JsonValue.Of(("checked", JsonValue.Of(passed.Select(p => JsonValue.Of(p)))));

    static StepReport Done(List<string> passed) => StepReport.Done(Data(passed));

    static StepReport Failed(List<string> passed, BroMessage message) => new(Data(passed), Error: message.ToError());
}
