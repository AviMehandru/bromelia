import BroDomain
import BroFoundation
import BroPorts

/// Whether the disc was archived before (plan §20.1 Plan and Decide; today's checkAlreadyArchived).
public enum ArchiveCheck {
    /// The newest committed unit with this fingerprint that still counts: its status is success, its library is online
    /// and its folder is there (read-error units, failures and vanished folders don't count). Nil without a fingerprint.
    public static func find(_ context: StepContext, _ fingerprint: String?) async throws(BroError) -> ArchivedMatch? {
        guard let fingerprint else { return nil }
        for unit in try await context.catalog.archivedBefore(fingerprint) {
            if unit.status != .success { continue }
            guard let root = try? context.library.require(unit.libraryId).path else { continue }
            var trimmed = Substring(root)
            while trimmed.last == "/" { trimmed = trimmed.dropLast() }
            let folder = String(trimmed) + "/" + unit.path
            if !context.fs.exists(folder) { continue }
            return ArchivedMatch(unitId: unit.id, folder: folder, day: String(Instant.format(unit.committedAt ?? unit.createdAt).prefix(10)))
        }
        return nil
    }

    /// What the job does about it. A rip started by hand, or automation.alreadyArchived ripAgain: logs disc.archivedBefore
    /// and goes on (nil). skip: the reason to skip the job, disc.alreadyArchived. ask: nil (Decide asks: `asks`).
    public static func apply(_ context: StepContext, _ match: ArchivedMatch, configId: String?) -> BroMessage? {
        let policy = ArchiveCheck.policy(context, configId)
        if policy == "skip" { return message(.discAlreadyArchived, match, withUnit: true) }
        if policy != "ask" { context.log.log(message(.discArchivedBefore, match, withUnit: false)) }
        return nil
    }

    /// Whether the job asks about a disc archived before: an automatic rip under automation.alreadyArchived ask.
    public static func asks(_ context: StepContext, configId: String?) -> Bool { policy(context, configId) == "ask" }

    private static func policy(_ context: StepContext, _ configId: String?) -> String? {
        context.job.request.automatic ? Config.automation(context.config, configId)["alreadyArchived"]?.string : "ripAgain"
    }

    /// The message about the unit: {folder, day}, and its unitId for a reason to skip.
    public static func message(_ code: MessageCode, _ match: ArchivedMatch, withUnit: Bool) -> BroMessage {
        var params: [(key: String, value: JsonValue)] = [("folder", .string(match.folder)), ("day", .string(match.day))]
        if withUnit { params.append(("unitId", .string(match.unitId.value))) }
        return BroMessage(code, params, severity: .warning)
    }
}
