using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>Step 2 (plan §20.1): reads the disc's listing with makemkvcon (disc.op.reading), under the profile the drive
/// gets before Plan. A disc opened in a session is read again (disc.readingAgain): ReconcileStep checks it is still
/// that disc. A listing without titles fails with disc.noTitles and MakeMKV's notice (an expired key, LibreDrive
/// required), except for a backup, which goes on as today with the opened listing (disc.usingOpenedListing) or without
/// one (disc.continuingWithoutListing). Logs disc.found, the LibreDrive detail and what the run's isolation or
/// transcript reported. Data: ProbeOutput.</summary>
public sealed class ProbeStep : IStep
{
    public StepKind Kind => StepKind.Probe;

    public Resumability Resumability => Resumability.Idempotent;

    public ResourceSet Resources(JobSnapshot job) =>
        DiscRequest.Decode(job.Request)?.Source is DiscSource.Drive d ? ResourceSet.None with { Drives = new[] { d.DriveId } } : ResourceSet.None;

    public async Task<StepReport> Run(StepContext context)
    {
        var request = StepSources.Request(context.Job);
        var source = StepSources.Makemkv(request.Source, context.DriveLookup);
        var configId = request.Source is DiscSource.Drive d ? context.DriveLookup.Find(d.DriveId)?.ConfigId : null;
        var profile = StepSources.Profile(context, configId);
        var operation = new BroMessage(MessageCode.DiscOpReading);
        if (request.Session != null) context.Log.Log(new BroMessage(MessageCode.DiscReadingAgain));
        context.Progress.Report(0, operation);
        var invocation = context.MakemkvSetup.Invocation(profile, null, context.JobDirectory);
        var read = await context.Tools.Makemkv.Listing(source, invocation, new MakemkvSink(context.Progress, context.Log, operation), context.Cancel);
        var run = read.Run;
        foreach (var problem in new[] { run.TranscriptProblem, run.SettingsProblem })
            if (problem != null) context.Log.Log(problem with { Severity = Severity.Warning });
        if (run.Outcome.Status == StatusWord.Cancelled || context.Cancel.IsCancelled)
            return new StepReport(Empty, Error: new BroError(MessageCode.Wire(MessageCode.JobCancelled)));
        if (run.LibreDrive is { } detail)
            context.Log.Log(new BroMessage(MessageCode.MakemkvLibreDrive, Severity.Info, ("detail", JsonValue.Of(detail))));

        var listing = read.Listing;
        if (listing.Titles.Count == 0)
        {
            var reason = run.Outcome.Error
                ?? new BroMessage(MessageCode.ProcessExitStatus, Severity.Error, ("status", JsonValue.Of(run.Outcome.ExitCode)));
            var noTitles = new BroMessage(MessageCode.DiscNoTitles, Severity.Error, ("source", JsonValue.Of(Describe(source))),
                ("reason", reason.ToJson()));
            if (StepSources.BaseMode(context.Job, profile) is not (RipMode.Backup or RipMode.BackupDecrypted))
                return new StepReport(Empty, Error: noTitles.ToError(), Notice: run.Notice);
            if (request.Session is { } session)
            {
                context.Log.Log(new BroMessage(MessageCode.DiscUsingOpenedListing, Severity.Warning, ("reason", noTitles.ToJson())));
                return Done(new ProbeOutput(session.Listing, Fingerprint.Of(session.Listing), run.LibreDrive, true));
            }
            context.Log.Log(new BroMessage(MessageCode.DiscContinuingWithoutListing, Severity.Warning, ("reason", noTitles.ToJson())));
            return Done(new ProbeOutput(null, null, run.LibreDrive, false));
        }
        context.Log.Log(new BroMessage(MessageCode.DiscFound, Severity.Info, ("count", JsonValue.Of(listing.Titles.Count)),
            ("name", JsonValue.Of(listing.Name))));
        return Done(new ProbeOutput(listing, Fingerprint.Of(listing), run.LibreDrive, false));
    }

    static StepReport Done(ProbeOutput output) => StepReport.Done(ProbeOutput.Encode(output));

    /// <summary>The source as makemkvcon names it: disc:N, iso:path or file:path.</summary>
    static string Describe(MakemkvSource source) => source switch
    {
        MakemkvSource.Drive d => "disc:" + d.Index.ToString(System.Globalization.CultureInfo.InvariantCulture),
        MakemkvSource.Iso i => "iso:" + i.Path,
        MakemkvSource.File f => "file:" + f.Path,
        _ => "",
    };

    static readonly JsonValue Empty = JsonValue.Of(System.Array.Empty<(string, JsonValue)>());
}
