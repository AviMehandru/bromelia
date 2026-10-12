using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>Step 1 of an automatic rip (plan §20.1): waits up to the drive's automation.waitForMountSeconds for the OS
/// to mount the disc, then goes on either way (disc.mounted / disc.notMounted). Fails with disc.changed when another
/// disc was inserted since the job was queued. Skipped when the job isn't on a drive, the drive has no device or the
/// wait is 0. Data: {mountPath}.</summary>
public sealed class AwaitMediaStep : IStep
{
    public StepKind Kind => StepKind.AwaitMedia;

    public Resumability Resumability => Resumability.Idempotent;

    public ResourceSet Resources(JobSnapshot job) =>
        job.Request.DriveId is { } d ? ResourceSet.None with { Drives = new[] { d } } : ResourceSet.None;

    public async Task<StepReport> Run(StepContext context)
    {
        if (StepSources.Request(context.Job).Source is not DiscSource.Drive drive) return StepReport.Skip;
        var at = StepSources.Drive(context.DriveLookup, drive.DriveId);
        if (context.Job.Request.MediaGeneration is { } generation && at.Generation != generation)
            return Failed(new BroError(MessageCode.Wire(MessageCode.DiscChanged),
                ("why", new BroMessage(MessageCode.DiscChangedGeneration).ToJson())));
        long seconds = Config.Automation(context.Config, at.ConfigId)["waitForMountSeconds"]?.AsInteger ?? 0;
        if (seconds <= 0 || at.Device.Length == 0) return StepReport.Skip;
        context.Progress.Report(0, new BroMessage(MessageCode.DiscWaitingForMount));
        var mount = await context.Drives.WaitForMount(at.Device, new Duration(seconds), context.Cancel);
        if (context.Cancel.IsCancelled) return Failed(new BroError(MessageCode.Wire(MessageCode.JobCancelled)));
        context.Log.Log(mount != null ? new BroMessage(MessageCode.DiscMounted)
            : new BroMessage(MessageCode.DiscNotMounted, Severity.Info, ("seconds", JsonValue.Of(seconds))));
        return StepReport.Done(JsonValue.Of(("mountPath", JsonValue.Of(mount))));
    }

    static StepReport Failed(BroError error) => new(JsonValue.Of(System.Array.Empty<(string, JsonValue)>()), Error: error);
}
