using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>Step 5 (plan §20.1), its phase 3 form: Identity.resolve from the listing Probe read (or none), the disc's
/// label, the drive's flags and the name and kind chosen in the session; a backup that stays encrypted (mode backup
/// before Plan) gets the e format code. Online lookups, the archive's previous disc and menu numbers join in phase 5.
/// Logs identity.resolved. Data: the Identity.</summary>
public sealed class IdentifyStep : IStep
{
    public StepKind Kind => StepKind.Identify;

    public Resumability Resumability => Resumability.Idempotent;

    public ResourceSet Resources(JobSnapshot job) => ResourceSet.None;

    public Task<StepReport> Run(StepContext context)
    {
        var request = StepSources.Request(context.Job);
        var drive = request.Source is DiscSource.Drive d ? context.DriveLookup.Find(d.DriveId) : null;
        var mode = StepSources.BaseMode(context.Job, StepSources.Profile(context, drive?.ConfigId));
        var identity = Resolve(context, mode == RipMode.Backup);
        var set = LabelParser.SetDescription(identity.Label);
        context.Log.Log(new BroMessage(MessageCode.IdentityResolved, Severity.Info, ("kind", JsonValue.Of(EnumWire.Name(identity.Kind))),
            ("name", JsonValue.Of(identity.Name)), ("hasSet", JsonValue.Of(set.Length > 0)), ("set", JsonValue.Of(set)),
            ("format", JsonValue.Of(EnumWire.Name(identity.Format))), ("formatCode", JsonValue.Of(identity.FormatCode.Text)),
            ("reason", identity.Reason.ToJson())));
        return Task.FromResult(StepReport.Done(Identity.Encode(identity)));
    }

    /// <summary>The identity of the job's disc from Probe's listing (or none), the disc's label (the listing's name, else
    /// StepSources.label), the drive's flags and the session's name and kind; <paramref name="encrypted"/>: a backup that
    /// stays encrypted. Plan resolves it again under the mode it chose.</summary>
    public static Identity Resolve(StepContext context, bool encrypted)
    {
        var request = StepSources.Request(context.Job);
        var probe = ProbeOutput.Decode(JobSnapshot.Output(context.Job, StepKind.Probe) ?? JsonValue.Null.Instance);
        var drive = request.Source is DiscSource.Drive d ? context.DriveLookup.Find(d.DriveId) : null;
        var label = probe.Listing is { Name.Length: > 0 } l ? l.Name : StepSources.Label(request.Source, drive);
        var choices = request.Session?.Choices;
        return Identity.Resolve(new IdentityInputs(probe.Listing, label, encrypted, drive?.Flags,
            NameOverride: choices?.Name ?? "", KindOverride: choices?.Kind));
    }
}
