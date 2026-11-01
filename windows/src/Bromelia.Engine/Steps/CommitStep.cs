using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Engine;

/// <summary>Step 14 (plan §20.1, §15.4; §22.3 steps 4–6 through LibraryService): moves the unit Seal made in the job's
/// staging folder into the library. Nothing is hashed again: the files are Seal's (unit_files rows), the record Seal's
/// data. The intent's items are Name's, by the names they have in staging (each in place: staging is laid out as the
/// unit's folder), a folder told from Seal's files, then Bromelia's own files Seal wrote (the record, the logs, the note);
/// its destination is &lt;library&gt;/&lt;Name's folder&gt;, merged item by item when Name says the folder is shared. A
/// unit Seal tagged (READ ERRORS) goes to _Quarantine/&lt;Name's name&gt; [&lt;tag&gt;] instead (commit.quarantined), never
/// merged, and the step reports it quarantined; otherwise archive.moved. A folder of its own that another job committed
/// after Name ran takes the next free name (archive.outputFolder, a warning), unless naming.conflictPolicy is skip (owner
/// H3b-2), and never a name the library has called taken. LibraryService waits while a roll forward of
/// the unit runs (the job's cancel ends that wait), finishes an open intent of the unit and gives a unit committed already
/// its result again, so a rerun rolls forward. Holds the library write. Data: CommitOutput. Skipped when Plan or Seal hasn't
/// run.</summary>
public sealed class CommitStep : IStep
{
    const string QuarantineFolder = "_Quarantine";

    public StepKind Kind => StepKind.Commit;

    /// <summary>Plan §20.1's "rolls forward": a rerun finds the unit's open intent and finishes it.</summary>
    public Resumability Resumability => Resumability.Idempotent;

    public ResourceSet Resources(JobSnapshot job) => ResourceSet.None with
    {
        LibraryWrites = PlanOutput.Of(job) is { LibraryId.Length: > 0 } plan ? new[] { plan.LibraryId } : Array.Empty<string>(),
    };

    public async Task<StepReport> Run(StepContext context)
    {
        var job = context.Job;
        if (PlanOutput.Of(job) is not { } plan || SealOutput.Of(job) is not { } seal) return StepReport.Skip;
        var named = NameOutput.Of(job) ?? throw new BroFailure(new BroMessage(MessageCode.NameNoFolder, Severity.Error).ToError());
        var root = context.Library.Require(plan.LibraryId).Path;
        if (context.Cancel.IsCancelled) throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.JobCancelled)));
        var staging = context.Library.StagingDirectory(plan.LibraryId, job.Id);

        bool quarantine = seal.Tag != null;
        var path = quarantine ? QuarantineFolder + "/" + named.Name : named.Folder;
        var now = context.Clock.Now();
        var intent = new CommitIntent(seal.UnitId, job.Id, staging, root + "/" + path, named.Merge && !quarantine, false, now, Items(named, seal));
        var unit = Unit(job, plan, named, seal, path, now);
        context.Progress.Report(0, new BroMessage(MessageCode.ArchiveMoving));
        CommitResult result;
        if (quarantine) result = await context.Library.Quarantine(unit, intent, seal.Files, seal.Tag!, context.Cancel);
        else
        {
            // A folder of its own that another job committed after Name ran (two discs of the same name at once): the next
            // free name, as Name would have given it, unless the profile says skip (owner H3b-2). The library refuses a
            // taken folder before it writes anything, and an open intent or a unit committed already never gets that far.
            bool free = !named.Merge && NamingSettings.Decode(plan.Profile["naming"] ?? JsonValue.Of()).ConflictPolicy != ConflictPolicy.Skip;
            // Each name the library called taken, the first being Name's: never asked for again, even when the listing
            // shows it under other bytes (a file system that ignores case beyond ASCII, or normalization).
            var refused = new List<string>();
            while (true)
            {
                try
                {
                    result = await context.Library.Commit(unit, intent, seal.Files, context.Cancel);
                    break;
                }
                catch (BroFailure f) when (free && f.Error.Code == MessageCode.Wire(MessageCode.LibraryFolderExists)
                    && f.Error.Params["path"]?.AsString == intent.Destination)
                {
                    var parent = path.Contains('/') ? path[..path.LastIndexOf('/')] : "";
                    var dir = parent.Length > 0 ? root + "/" + parent : root;
                    refused.Add(path[(path.LastIndexOf('/') + 1)..]);
                    var name = ConflictNamer.Next(refused[0], context.Fs.List(dir).Select(e => e.Name).Concat(refused).ToList(), isFolder: true);
                    path = parent.Length > 0 ? parent + "/" + name : name;
                    intent = intent with { Destination = root + "/" + path };
                    unit = unit with { Path = path };
                    context.Log.Log(new BroMessage(MessageCode.ArchiveOutputFolder, Severity.Warning, ("path", JsonValue.Of(intent.Destination))));
                }
            }
        }

        context.Log.Log(quarantine
            ? new BroMessage(MessageCode.CommitQuarantined, Severity.Warning, ("path", JsonValue.Of(result.Destination)))
            : new BroMessage(MessageCode.ArchiveMoved, Severity.Info, ("count", JsonValue.Of(intent.Items.Count)), ("path", JsonValue.Of(result.Destination))));
        var committed = result.Destination.StartsWith(root + "/", StringComparison.Ordinal) ? result.Destination[(root.Length + 1)..] : result.Destination;
        return new StepReport(CommitOutput.Encode(new CommitOutput(seal.UnitId, plan.LibraryId, committed, result.Merged, quarantine)),
            Quarantined: quarantine);
    }

    /// <summary>Name's items in order, each in place (a folder when Seal hashed no file of that name), then the record,
    /// the logs and the note Seal wrote. Only files carry a SHA-256 (Seal's): Bromelia's own files aren't in SHA256SUMS,
    /// and their names hold the unit's id.</summary>
    static IReadOnlyList<CommitItem> Items(NameOutput named, SealOutput seal)
    {
        var items = new List<CommitItem>();
        foreach (var item in named.Files)
        {
            var path = item.Path.TrimEnd('/');
            var file = seal.Files.FirstOrDefault(f => f.Path == path);
            items.Add(new CommitItem(items.Count, path, path, file?.Sha256, file == null, false));
        }
        foreach (var own in seal.Logs.Prepend(seal.Record).Concat(seal.Note is { } note ? new[] { note } : Array.Empty<string>()))
            items.Add(new CommitItem(items.Count, own, own, null, false, false));
        return items;
    }

    /// <summary>The archive_units row, from what Seal's record holds: its identity (Name's when it corrected it, else
    /// Plan's), the disc's label (the listing's name, else the source's), Probe's fingerprint and MakeMKV version, Seal's
    /// status, record and files.</summary>
    static UnitRecord Unit(JobSnapshot job, PlanOutput plan, NameOutput named, SealOutput seal, string path, Instant now)
    {
        var probe = JobSnapshot.Output(job, StepKind.Probe) is JsonValue.Object p ? ProbeOutput.Decode(p) : null;
        var identity = named.Identity ?? plan.Identity;
        var source = DiscRequest.Decode(job.Request)?.Source;
        var label = probe?.Listing is { Name.Length: > 0 } listing ? listing.Name : source != null ? StepSources.Label(source, null) : "";
        return new UnitRecord(seal.UnitId, plan.LibraryId, null, job.Id, path, seal.Record, 3, UnitState.Committing,
            EnumWire.Parse<UnitStatus>(seal.Status) ?? UnitStatus.Success, identity.Name, identity.Kind, EnumWire.Name(identity.Format),
            identity.FormatCode.Text, identity.Encrypted, probe?.Fingerprint, label, identity.Label.Season, identity.Label.Part,
            identity.Label.Volume, identity.Label.Disc, probe?.MakemkvVersion ?? "", seal.Files.Sum(f => f.Size), seal.Files.Count, 1, now);
    }
}
