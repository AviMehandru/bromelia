using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>Step 3 (plan §12.4, §20.1), only when a session made the choices: the disc must still be the one that was
/// opened (disc.changed with why), and each ticked title must be found in the listing read now
/// (ListingMatch.mapTitles; hand-picked tracks must not have changed). Logs disc.titleNumbersChanged when MakeMKV
/// numbers them differently now. Skipped without a session, or when Probe had to use the opened listing. Data:
/// ReconcileOutput.</summary>
public sealed class ReconcileStep : IStep
{
    public StepKind Kind => StepKind.Reconcile;

    public Resumability Resumability => Resumability.Idempotent;

    public ResourceSet Resources(JobSnapshot job) => ResourceSet.None;

    public Task<StepReport> Run(StepContext context)
    {
        if (StepSources.Request(context.Job).Session is not { } session) return Task.FromResult(StepReport.Skip);
        var probe = ProbeOutput.Decode(JobSnapshot.Output(context.Job, StepKind.Probe) ?? JsonValue.Null.Instance);
        if (probe.FromSession || probe.Listing is not { } fresh) return Task.FromResult(StepReport.Skip);
        if (ListingMatch.DifferentDisc(session.Listing, fresh) is { } why)
            return Task.FromResult(Failed(new BroError(MessageCode.Wire(MessageCode.DiscChanged), ("why", why.ToJson()))));
        Dictionary<int, int> map;
        try
        {
            map = ListingMatch.MapTitles(session.Choices.Titles.Where(t => t.Selected).Select(t => t.Index), session.Listing, fresh,
                session.Choices.Titles.Where(t => t.Tracks != null).Select(t => t.Index).ToHashSet());
        }
        catch (BroFailure f)
        {
            return Task.FromResult(Failed(f.Error));
        }
        var moved = map.Where(kv => kv.Key != kv.Value).OrderBy(kv => kv.Key)
            .Select(kv => JsonValue.Of(FormattableStringInvariant(kv.Key, kv.Value))).ToList();
        if (moved.Count > 0) context.Log.Log(new BroMessage(MessageCode.DiscTitleNumbersChanged, Severity.Info, ("changes", JsonValue.Of(moved))));
        return Task.FromResult(StepReport.Done(ReconcileOutput.Encode(new ReconcileOutput(map))));
    }

    static string FormattableStringInvariant(int from, int to) => System.FormattableString.Invariant($"{from} → {to}");

    static StepReport Failed(BroError error) => new(JsonValue.Of(System.Array.Empty<(string, JsonValue)>()), Error: error);
}
