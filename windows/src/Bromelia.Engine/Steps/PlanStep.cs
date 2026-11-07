using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>Step 6 (plan §20.1): decides what the job makes and appends the rest of its steps: the profile
/// (ProfileResolver.resolve; a session's choices, else the request's options), the mode (the request's; MKV when titles
/// were chosen by hand; else ModeChooser by the disc's format), the titles (the session's ticked ones, numbered as
/// Reconcile found them, else TitleRules.select), the library and the space.
///
/// A mode that makes MKV files needs the listing and titles. An offline library doesn't fail the job: Decide waits for
/// it, blocked, then checks the archive and the space. A disc archived before ends the job under skip and appends
/// Decide under ask; the space is reserved here only when no Decide follows.
///
/// Appends [Decide?], Acquire, Release, VerifyRips (archive.verifyRips, not for infoOnly), Name, Seal, Commit, Notify.
/// Data: PlanOutput.</summary>
public sealed class PlanStep : IStep
{
    public StepKind Kind => StepKind.Plan;

    public Resumability Resumability => Resumability.Idempotent;

    public ResourceSet Resources(JobSnapshot job) => ResourceSet.None;

    public async Task<StepReport> Run(StepContext context)
    {
        var job = context.Job;
        var request = StepSources.Request(job);
        var probe = ProbeOutput.Decode(JobSnapshot.Output(job, StepKind.Probe) ?? JsonValue.Null.Instance);
        var identity = Identity.Decode(JobSnapshot.Output(job, StepKind.Identify) ?? JsonValue.Of());
        var drive = request.Source is DiscSource.Drive d ? context.DriveLookup.Find(d.DriveId) : null;
        var configId = drive?.ConfigId;
        var choices = request.Session?.Choices;
        var listing = probe.Listing;

        var label = listing is { Name.Length: > 0 } l ? l.Name : StepSources.Label(request.Source, drive);
        var facts = new RuleFacts(identity.Name, label, identity.FormatCode.Text, EnumWire.Name(identity.Kind), Automatic: job.Request.Automatic);
        var chosenProfile = choices?.Profile ?? job.Request.Options?["profile"]?.AsString;
        var chosenLibrary = choices?.Library ?? job.Request.Options?["library"]?.AsString;
        var profile = ProfileResolver.Resolve(context.Config, configId, facts, StepSources.Layer(context.Config, chosenProfile, chosenLibrary));
        var profileId = chosenProfile ?? profile.Trace.LastOrDefault(t => t.Layer == ResolveLayer.DriveProfile)?.Id
            ?? profile.Trace.First(t => t.Layer == ResolveLayer.DefaultProfile).Id ?? "default";
        bool byHand = choices != null && choices.Titles.Any(t => t.Selected);
        var mode = EnumWire.Parse<RipMode>(job.Request.Options?["mode"]?.AsString)
            ?? (byHand ? RipMode.Mkv : ModeChooser.Mode(identity.Format, drive?.Flags ?? new DiscFlags(0), DiscContent.Video, new Profile(profile.Profile), false))
            ?? RipMode.Mkv;
        if ((mode == RipMode.Backup) != identity.Encrypted) identity = IdentifyStep.Resolve(context, mode == RipMode.Backup);

        var titles = new List<TitleChoice>();
        int? onePass = null;
        long? need = null;
        if (mode is RipMode.Mkv or RipMode.BackupThenMkv)
        {
            if (listing == null) return Failed(new BroMessage(MessageCode.DiscNoInformation, Severity.Error));
            if (byHand)
            {
                var map = JobSnapshot.Output(job, StepKind.Reconcile) is JsonValue.Object r ? ReconcileOutput.Decode(r).TitleMap : null;
                foreach (var t in choices!.Titles.Where(t => t.Selected))
                {
                    int index = map != null && map.TryGetValue(t.Index, out var now) ? now : t.Index;
                    if (listing.Titles.Any(x => x.Index == index)) titles.Add(t with { Index = index });
                }
                titles = titles.OrderBy(t => t.Index).ToList();
                if (titles.Count == 0) return Failed(new BroMessage(MessageCode.TitlesNoneChosenExist, Severity.Error));
            }
            else
            {
                var selection = TitleRules.Select(listing, TitleSettings.Decode(profile.Profile["titles"] ?? JsonValue.Of()));
                if (selection.Error is { } error) return Failed(error);
                if (selection.RequiresManualChoice)
                    return Failed(new BroMessage(MessageCode.TitlesManualRequired, Severity.Error, ("profile", JsonValue.Of(ProfileName(context.Config, profileId)))));
                foreach (var trace in selection.Trace)
                {
                    var t = listing.Titles.First(x => x.Index == trace.Index);
                    var p = new List<(string, JsonValue)> { ("title", JsonValue.Of(t.Index)), ("duration", JsonValue.Of(t.Duration)), ("chapters", JsonValue.Of(t.Chapters)) };
                    if (!trace.Selected) p.Add(("reason", trace.Reason.ToJson()));
                    context.Log.Log(new BroMessage(trace.Selected ? MessageCode.TitlesSelected : MessageCode.TitlesSkipped, Severity.Info, p.ToArray()));
                }
                if (selection.Indices.Count == 0) return Failed(new BroMessage(MessageCode.TitlesNoneMatched, Severity.Error));
                titles = selection.Indices.Select(i => new TitleChoice(i, true)).ToList();
            }
            var picked = titles.Where(t => t.Tracks != null).Select(t => t.Index).ToHashSet();
            if (picked.Count == 0)
                onePass = OnePass.Plan(titles.Select(t => t.Index).ToList(), listing, (int?)profile.Profile["makemkv"]?["minLengthSeconds"]?.AsInteger)?.MinLength;
            var chosen = listing.Titles.Where(t => titles.Any(c => c.Index == t.Index)).ToList();
            long bytes = SpaceEstimate.ForPlan(chosen, picked, null).Count;
            need = bytes > 0 ? bytes : null;
        }

        var libraryId = profile.Profile["library"]?.AsString ?? context.Config.Document["defaultLibrary"]?.AsString
            ?? Config.Libraries(context.Config).FirstOrDefault()?.Id;
        if (libraryId == null) return Failed(new BroMessage(MessageCode.LibraryNone, Severity.Error));
        LibraryHandle? library = null;
        try
        {
            library = context.Library.Require(libraryId);
        }
        catch (BroFailure f) when (f.Error.Code != MessageCode.Wire(MessageCode.LibraryNotFound))
        {
            // Offline: Decide waits for it.
        }

        ArchivedMatch? ask = null;
        BroMessage? skip = null;
        if (library != null && mode != RipMode.InfoOnly && await ArchiveCheck.Find(context, probe.Fingerprint) is { } match)
        {
            skip = ArchiveCheck.Apply(context, match, configId);
            if (ArchiveCheck.Asks(context, configId)) ask = match;
        }
        bool decide = library == null || ask != null;
        bool reserved = false;
        if (skip == null && !decide && need is { } n)
        {
            await SpaceCheck.Reserve(context, library!.Path, n);
            reserved = true;
        }

        var output = new PlanOutput(mode, profileId, profile.Profile, libraryId, library == null, titles, onePass, need, reserved, ask, identity);
        var plan = new JobPlan(new List<StepKind>(), EnumWire.Name(mode), profileId, libraryId);
        if (skip != null) return new StepReport(PlanOutput.Encode(output), SkipJob: skip, Plan: plan);
        var steps = new List<StepKind>();
        if (decide) steps.Add(StepKind.Decide);
        steps.Add(StepKind.Acquire);
        steps.Add(StepKind.Release);
        if (mode != RipMode.InfoOnly && profile.Profile["archive"]?["verifyRips"]?.AsBool != false) steps.Add(StepKind.VerifyRips);
        steps.AddRange(new[] { StepKind.Name, StepKind.Seal, StepKind.Commit, StepKind.Notify });
        return new StepReport(PlanOutput.Encode(output), AppendSteps: steps, Plan: plan);
    }

    static string ProfileName(Config config, string profileId) =>
        Config.Profiles(config).FirstOrDefault(p => p.Id == profileId)?.Json["name"]?.AsString is { Length: > 0 } name ? name : profileId;

    static StepReport Failed(BroMessage message) => new(JsonValue.Of(), Error: message.ToError());
}
