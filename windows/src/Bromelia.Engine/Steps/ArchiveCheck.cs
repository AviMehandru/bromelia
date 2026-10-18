using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Engine;

/// <summary>Whether the disc was archived before (plan §20.1 Plan and Decide; today's checkAlreadyArchived).</summary>
public static class ArchiveCheck
{
    /// <summary>The newest committed unit with this fingerprint that still counts: its status is success, its library is
    /// online and its folder is there (read-error units, failures and vanished folders don't count). Null without a
    /// fingerprint.</summary>
    public static async Task<ArchivedMatch?> Find(StepContext context, string? fingerprint)
    {
        if (fingerprint == null) return null;
        foreach (var unit in await context.Catalog.ArchivedBefore(fingerprint))
        {
            if (unit.Status != UnitStatus.Success) continue;
            string root;
            try
            {
                root = context.Library.Require(unit.LibraryId).Path;
            }
            catch (BroFailure)
            {
                continue;
            }
            var folder = root.TrimEnd('/') + "/" + unit.Path;
            if (!context.Fs.Exists(folder)) continue;
            return new ArchivedMatch(unit.Id, folder, Instant.Format(unit.CommittedAt ?? unit.CreatedAt).Substring(0, 10));
        }
        return null;
    }

    /// <summary>What the job does about it. A rip started by hand, or automation.alreadyArchived ripAgain: logs
    /// disc.archivedBefore and goes on (null). skip: the reason to skip the job, disc.alreadyArchived. ask: null (Decide
    /// asks: <see cref="Asks"/>).</summary>
    public static BroMessage? Apply(StepContext context, ArchivedMatch match, string? configId)
    {
        var policy = Policy(context, configId);
        if (policy == "skip") return Message(MessageCode.DiscAlreadyArchived, match, true);
        if (policy != "ask") context.Log.Log(Message(MessageCode.DiscArchivedBefore, match, false));
        return null;
    }

    /// <summary>Whether the job asks about a disc archived before: an automatic rip under automation.alreadyArchived
    /// ask.</summary>
    public static bool Asks(StepContext context, string? configId) => Policy(context, configId) == "ask";

    static string? Policy(StepContext context, string? configId) =>
        context.Job.Request.Automatic ? Config.Automation(context.Config, configId)["alreadyArchived"]?.AsString : "ripAgain";

    /// <summary>The message about the unit: {folder, day}, and its unitId for a reason to skip.</summary>
    public static BroMessage Message(MessageCode code, ArchivedMatch match, bool withUnit) => withUnit
        ? new BroMessage(code, Severity.Warning, ("folder", JsonValue.Of(match.Folder)), ("day", JsonValue.Of(match.Day)),
            ("unitId", JsonValue.Of(match.UnitId.Value)))
        : new BroMessage(code, Severity.Warning, ("folder", JsonValue.Of(match.Folder)), ("day", JsonValue.Of(match.Day)));
}
