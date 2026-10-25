using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Engine;

/// <summary>The step JobService.keepApart puts in place of an interrupted job's interrupted step and what follows (owner,
/// 2026-10-03 and 2026-10-25; plan §20.1, §20.4): the job isn't continued, and what it left in staging becomes a sealed
/// unit in &lt;library&gt;/_Quarantine/&lt;name&gt; [INCOMPLETE] with a quarantined row of status incomplete.
/// <list type="number">
/// <item>Sealed as Seal seals, but with no produced lists (Acquire never finished): every visible file in staging (none
/// hidden by its first byte, not one of Bromelia's own at the top; leftovers of a stopped write removed) is synced, then
/// each folder down to it (archive.synced), and hashed read back from disk (archive.hashFromDisk) with archive.hashing
/// progress. SHA256SUMS, the logs as Seal copies them, INCOMPLETE.txt (the note recovery writes, with the logs) and the
/// record (status incomplete, no titles acquired) are written atomically; an earlier attempt's own files that this one
/// doesn't write go. Staging with no such file holds nothing to keep: its empty folders are removed and no unit is
/// made.</item>
/// <item>The checkpoint "sealed" (the files and Bromelia's own files) is saved before the unit is handed to the library: a
/// rerun with it doesn't seal again (staging may already be moving) and hands over the same unit.</item>
/// <item>library.quarantine(unit, intent, files, "INCOMPLETE"): the intent's items are staging's top-level entries that
/// hold the files (a file with its SHA-256, a folder without), then the record, the logs and the note; LibraryService
/// picks the free name, finishes an open intent and gives a unit quarantined already its result again, so a rerun rolls
/// forward. recovery.keptApart {path} (warning).</item>
/// </list>
/// &lt;name&gt;: Plan's identity name, else the request's title, else the listing's name, else the source's label, else the
/// job id, the first that stays non-empty as a path component. Holds the library write; idempotent. Data: CommitOutput
/// (quarantined). Skipped without Plan's output. JobOutcome makes the job cancelled once the step succeeded.</summary>
public sealed class KeepApartStep : IStep
{
    public const string Tag = "INCOMPLETE";
    public const string Note = "INCOMPLETE.txt";
    public const string SealedCheckpoint = "sealed";
    const string QuarantineFolder = "_Quarantine";

    public StepKind Kind => StepKind.KeepApart;

    public Resumability Resumability => Resumability.Idempotent;

    public ResourceSet Resources(JobSnapshot job) => ResourceSet.None with
    {
        LibraryWrites = PlanOutput.Of(job) is { LibraryId.Length: > 0 } plan ? new[] { plan.LibraryId } : Array.Empty<string>(),
    };

    public async Task<StepReport> Run(StepContext context)
    {
        var job = context.Job;
        if (PlanOutput.Of(job) is not { } plan) return StepReport.Skip;
        var root = context.Library.Require(plan.LibraryId).Path;
        SealStep.Cancelled(context);
        var staging = context.Library.StagingDirectory(plan.LibraryId, job.Id);
        var unitId = ArchiveFiles.UnitId(job.Id);

        // The unit an earlier attempt sealed; one that names none of Bromelia's own files isn't (nothing writes one).
        var saved = context.Checkpoint.Saved is { } c && c["kind"]?.AsString == SealedCheckpoint && c["own"]?.AsArray is { Count: > 0 } ? c : null;
        var (files, own) = saved != null ? FromCheckpoint(saved) : await Seal(context, plan, staging, unitId);
        if (files.Count == 0) return StepReport.Done(JsonValue.Of(Array.Empty<(string, JsonValue)>()));

        var probe = JobSnapshot.Output(job, StepKind.Probe) is JsonValue.Object p ? ProbeOutput.Decode(p) : null;
        var source = DiscRequest.Decode(job.Request)?.Source;
        var label = probe?.Listing is { Name.Length: > 0 } listing ? listing.Name : source != null ? StepSources.Label(source, null) : "";
        var name = new[] { plan.Identity.Name, job.Request.Title, probe?.Listing?.Name ?? "", source != null ? StepSources.Label(source, null) : "", job.Id.Value }
            .Select(Sanitizer.Component).First(n => n.Length > 0);
        var path = QuarantineFolder + "/" + name;
        var now = context.Clock.Now();
        var identity = plan.Identity;
        var unit = new UnitRecord(unitId, plan.LibraryId, null, job.Id, path, own[0], 3, UnitState.Committing, UnitStatus.Incomplete, identity.Name,
            identity.Kind, EnumWire.Name(identity.Format), identity.FormatCode.Text, identity.Encrypted, probe?.Fingerprint, label, identity.Label.Season,
            identity.Label.Part, identity.Label.Volume, identity.Label.Disc, probe?.MakemkvVersion ?? "", files.Sum(f => f.Size), files.Count, 1, now);
        var intent = new CommitIntent(unitId, job.Id, staging, root + "/" + path, false, false, now, Items(files, own));

        context.Progress.Report(0, new BroMessage(MessageCode.ArchiveMoving));
        var result = await context.Library.Quarantine(unit, intent, files, Tag, context.Cancel);
        context.Log.Log(new BroMessage(MessageCode.RecoveryKeptApart, Severity.Warning, ("path", JsonValue.Of(result.Destination))));
        var kept = result.Destination.StartsWith(root + "/", StringComparison.Ordinal) ? result.Destination[(root.Length + 1)..] : result.Destination;
        return StepReport.Done(CommitOutput.Encode(new CommitOutput(unitId, plan.LibraryId, kept, false, true)));
    }

    /// <summary>Seals what staging holds; its files (none: nothing to keep, staging's empty folders removed) and Bromelia's
    /// own files (the record first), once the checkpoint that names them is stored.</summary>
    static async Task<(IReadOnlyList<UnitFile> Files, IReadOnlyList<string> Own)> Seal(StepContext context, PlanOutput plan, string staging, Id unitId)
    {
        var fs = context.Fs;
        var archive = plan.Profile["archive"];
        bool hashFromDisk = archive?["hashFromDisk"]?.AsBool ?? true;
        bool copyLogs = archive?["archiveRecord"]?.AsBool ?? true;
        var tree = new List<string>();
        foreach (var f in FileTree.Files(fs, staging))
        {
            if (!f.EndsWith('/') && ArchiveFiles.IsLeftover(f[(f.LastIndexOf('/') + 1)..])) fs.Remove(staging + "/" + f);
            else tree.Add(f);
        }
        var paths = tree.Where(f => !f.EndsWith('/') && !f.Split('/').Any(c => c.StartsWith('.')) && !(!f.Contains('/') && ArchiveFiles.IsOwnFile(f)))
            .ToList();
        if (paths.Count == 0)
        {
            RemoveEmpty(fs, staging);
            return (Array.Empty<UnitFile>(), Array.Empty<string>());
        }

        SealStep.Cancelled(context);
        foreach (var f in paths) fs.SyncFile(staging + "/" + f);
        foreach (var folder in paths.SelectMany(f => SealStep.Folders(staging, f)).Distinct().OrderBy(f => f, StringComparer.Ordinal))
            fs.SyncDirectory(folder);
        context.Log.Log(new BroMessage(MessageCode.ArchiveSynced, Severity.Info, ("count", JsonValue.Of(paths.Count))));

        var sizes = paths.Select(f => fs.Stat(staging + "/" + f).Size).ToList();
        long total = Math.Max(1, sizes.Sum()), done = 0;
        var files = new List<UnitFile>();
        for (int i = 0; i < paths.Count; i++)
        {
            SealStep.Cancelled(context);
            context.Progress.Report((double)done / total, new BroMessage(MessageCode.ArchiveHashing), paths[i]);
            var (hash, size) = SealStep.Hash(context, staging + "/" + paths[i], paths[i], hashFromDisk);
            files.Add(new UnitFile(paths[i], size, hash, ""));
            done += sizes[i];
        }
        SealStep.Cancelled(context);

        var unit8 = unitId.Value[..8];
        var written = new List<string>();
        void Write(string name, byte[] bytes)
        {
            SealStep.WriteOwn(context, staging + "/" + name, bytes);
            written.Add(name);
        }
        Write(Sha256Sums.FileName, Encoding.UTF8.GetBytes(Sha256Sums.Render(files.Select(f => new SumEntry(f.Path, f.Sha256)))));
        context.Log.Log(new BroMessage(MessageCode.ArchiveChecksumsWritten, Severity.Info, ("count", JsonValue.Of(files.Count)),
            ("file", JsonValue.Of(Sha256Sums.FileName))));
        var (log, logs) = SealStep.CopyLogs(context, unit8, copyLogs, Write);
        var lines = Notes.Render(context.Job.Id, Outcome.Interrupted, null, Array.Empty<RobotMessage>(), logs);
        Write(Note, Encoding.UTF8.GetBytes(string.Concat(lines.Select(l => context.Messages.Render(l) + "\n"))));
        var record = "bromelia-" + unit8 + ".json";
        var acquired = new AcquireOutput(new List<AcquiredTitle>(), new List<string>(), null, null, new List<string>());
        var document = SealRecord.Build(context.Job, plan, acquired, unitId, files, log, logs, context.Clock.Now(), context.AppVersion, context.Messages,
            "incomplete");
        Write(record, ArchiveRecordCodec.EncodeV3(new ArchiveRecord(3, document, "", "", "", "", "", null, null, null, null, null, null,
            new List<int>(), new List<SumEntry>())));
        context.Log.Log(new BroMessage(MessageCode.ArchiveRecordWritten, Severity.Info, ("file", JsonValue.Of(record))));
        foreach (var f in tree.Where(f => !f.Contains('/') && ArchiveFiles.IsOwnFile(f) && !written.Contains(f)))
            fs.Remove(staging + "/" + f);
        fs.SyncDirectory(staging);

        var own = new List<string> { record };
        own.AddRange(logs);
        own.Add(Note);
        await context.Checkpoint.Save(JsonValue.Of(("kind", JsonValue.Of(SealedCheckpoint)),
            ("files", JsonValue.Of(files.Select(f => JsonValue.Of(("path", JsonValue.Of(f.Path)), ("size", JsonValue.Of(f.Size)),
                ("sha256", JsonValue.Of(f.Sha256)))))),
            ("own", JsonValue.Of(own.Select(o => JsonValue.Of(o))))));
        return (files, own);
    }

    static (IReadOnlyList<UnitFile> Files, IReadOnlyList<string> Own) FromCheckpoint(JsonValue saved) => (
        (saved["files"]?.AsArray ?? new List<JsonValue>())
            .Select(f => new UnitFile(f["path"]?.AsString ?? "", f["size"]?.AsInteger ?? 0, f["sha256"]?.AsString ?? "", "")).ToList(),
        (saved["own"]?.AsArray ?? new List<JsonValue>()).Select(o => o.AsString ?? "").ToList());

    /// <summary>Staging's top-level entries that hold the files, in ordinal order (a file with its SHA-256, a folder
    /// without), then Bromelia's own files; each in place.</summary>
    static IReadOnlyList<CommitItem> Items(IReadOnlyList<UnitFile> files, IReadOnlyList<string> own)
    {
        var items = new List<CommitItem>();
        foreach (var top in files.Select(f => f.Path.Split('/')[0]).Distinct().OrderBy(t => t, StringComparer.Ordinal))
        {
            var file = files.FirstOrDefault(f => f.Path == top);
            items.Add(new CommitItem(items.Count, top, top, file?.Sha256, file == null, false));
        }
        foreach (var o in own) items.Add(new CommitItem(items.Count, o, o, null, false, false));
        return items;
    }

    /// <summary>Removes the folder's empty folders, and the folder itself once empty; a file stays.</summary>
    static void RemoveEmpty(IFileSystem fs, string folder)
    {
        foreach (var e in fs.List(folder).Where(e => e.IsDirectory)) RemoveEmpty(fs, folder + "/" + e.Name);
        if (fs.List(folder).Count == 0) fs.Remove(folder);
    }
}
