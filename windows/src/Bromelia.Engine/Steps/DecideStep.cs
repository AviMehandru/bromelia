using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>Step 7 (plan §20.1), appended by Plan only when needed. It holds the plan's library for writing, so while
/// that library is offline the Scheduler keeps the job blocked (library.offline) and it goes on by itself when the
/// library is back. Then:
/// <list type="bullet">
/// <item>When the library was offline at Plan: the archive check Plan couldn't make (ArchiveCheck: skip ends the job,
/// ask asks).</item>
/// <item>An archived disc under automation.alreadyArchived ask: decision.archivedBefore {folder, day}, answers ripAgain
/// (logs disc.archivedBefore), skip and eject (both end the job with disc.alreadyArchivedAsk; Release at the end
/// ejects the disc for eject).</item>
/// <item>The space Plan didn't reserve (SpaceCheck).</item>
/// </list>
/// Data: {answer} (null when nothing was asked).</summary>
public sealed class DecideStep : IStep
{
    public const string ArchivedBefore = "archivedBefore";

    public StepKind Kind => StepKind.Decide;

    public Resumability Resumability => Resumability.Idempotent;

    public ResourceSet Resources(JobSnapshot job) =>
        PlanOutput.Of(job) is { LibraryId.Length: > 0 } plan ? ResourceSet.None with { LibraryWrites = new[] { plan.LibraryId } } : ResourceSet.None;

    public async Task<StepReport> Run(StepContext context)
    {
        var plan = PlanOutput.Of(context.Job) ?? throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.InternalUnexpected),
            ("detail", JsonValue.Of("decide without a plan"))));
        var request = StepSources.Request(context.Job);
        var configId = request.Source is DiscSource.Drive d ? context.DriveLookup.Find(d.DriveId)?.ConfigId : null;
        var ask = plan.AskArchived;
        if (plan.LibraryOffline && plan.Mode != RipMode.InfoOnly)
        {
            var probe = ProbeOutput.Decode(JobSnapshot.Output(context.Job, StepKind.Probe) ?? JsonValue.Null.Instance);
            if (await ArchiveCheck.Find(context, probe.Fingerprint) is { } match)
            {
                if (ArchiveCheck.Apply(context, match, configId) is { } skip) return Answered(null, skip);
                if (ArchiveCheck.Asks(context, configId)) ask = match;
            }
        }
        string? answer = null;
        if (ask != null)
        {
            answer = await context.Decisions.Ask(new DecisionDto(ArchivedBefore, ArchiveCheck.Message(MessageCode.DecisionArchivedBefore, ask, false)
                with { Severity = Severity.Info }, new List<DecisionAnswer>
            {
                new("ripAgain", new BroMessage(MessageCode.DecisionAnswerRipAgain)),
                new("skip", new BroMessage(MessageCode.DecisionAnswerSkip)),
                new("eject", new BroMessage(MessageCode.DecisionAnswerEject)),
            }));
            if (answer != "ripAgain") return Answered(answer, ArchiveCheck.Message(MessageCode.DiscAlreadyArchivedAsk, ask, true));
            context.Log.Log(ArchiveCheck.Message(MessageCode.DiscArchivedBefore, ask, false));
        }
        if (!plan.SpaceReserved && plan.SpaceNeed is { } need) await SpaceCheck.Reserve(context, context.Library.Require(plan.LibraryId).Path, need);
        return Answered(answer, null);
    }

    static StepReport Answered(string? answer, BroMessage? skip) =>
        new(JsonValue.Of(("answer", answer is null ? JsonValue.Null.Instance : JsonValue.Of(answer))), SkipJob: skip);
}
