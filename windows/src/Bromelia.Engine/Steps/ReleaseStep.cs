using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>Step 9 (plan §20.1): ejects the disc when the drive's automation.ejectWhenDone says so (drive.ejecting,
/// drive.ejected). A failed eject is logged as drive.ejectFailed (a warning) and doesn't fail the job. The runner then
/// gives back the drive and moves the job to the processing queue: the drive takes the next disc. Skipped when the
/// job isn't on a drive. Data: {ejected}.</summary>
public sealed class ReleaseStep : IStep
{
    public StepKind Kind => StepKind.Release;

    public Resumability Resumability => Resumability.Idempotent;

    public ResourceSet Resources(JobSnapshot job) =>
        job.Request.DriveId is { } d ? ResourceSet.None with { Drives = new[] { d } } : ResourceSet.None;

    public async Task<StepReport> Run(StepContext context)
    {
        if (StepSources.Request(context.Job).Source is not DiscSource.Drive drive) return StepReport.Skip;
        var at = context.DriveLookup.Find(drive.DriveId);
        if (at is not { Device.Length: > 0 } || Config.Automation(context.Config, at.ConfigId)["ejectWhenDone"]?.AsBool != true)
            return Ejected(false);
        context.Progress.Report(0, new BroMessage(MessageCode.DriveEjecting));
        try
        {
            await context.Drives.Eject(at.Device);
        }
        catch (BroFailure f)
        {
            context.Log.Log(new BroMessage(MessageCode.DriveEjectFailed, Severity.Warning, ("device", JsonValue.Of(at.Device)),
                ("reason", JsonValue.Of(f.Error.Code))));
            return Ejected(false);
        }
        context.Log.Log(new BroMessage(MessageCode.DriveEjected));
        return Ejected(true);
    }

    static StepReport Ejected(bool ejected) => StepReport.Done(JsonValue.Of(("ejected", JsonValue.Of(ejected))));
}
