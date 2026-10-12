import BroDomain
import BroFoundation

/// Step 1 of an automatic rip (plan §20.1): waits up to the drive's automation.waitForMountSeconds for the OS to mount
/// the disc, then goes on either way (disc.mounted / disc.notMounted). Fails with disc.changed when another disc was
/// inserted since the job was queued. Skipped when the job isn't on a drive, the drive has no device or the wait is
/// 0. Data: {mountPath}.
public struct AwaitMediaStep: Step {
    public init() {}

    public var kind: StepKind { .awaitMedia }

    public var resumability: Resumability { .idempotent }

    public func resources(_ job: JobSnapshot) -> ResourceSet { ResourceSet(drives: job.request.driveId.map { [$0] } ?? []) }

    public func run(_ context: StepContext) async throws(BroError) -> StepReport {
        guard case .drive(let driveId) = try StepSources.request(context.job).source else { return StepReport(skipped: true) }
        let at = try StepSources.drive(context.driveLookup, driveId)
        if let generation = context.job.request.mediaGeneration, at.generation != generation {
            return StepReport(error: BroError(MessageCode.wire(.discChanged), [("why", BroMessage(.discChangedGeneration).toJson())]))
        }
        let seconds = Config.automation(context.config, at.configId)["waitForMountSeconds"]?.int ?? 0
        if seconds <= 0 || at.device.isEmpty { return StepReport(skipped: true) }
        context.progress.report(0, operation: BroMessage(.discWaitingForMount), current: nil)
        let mount = await context.drives.waitForMount(at.device, timeout: Duration(seconds: Double(seconds)), cancel: context.cancel)
        if context.cancel.isCancelled { return StepReport(error: BroError(MessageCode.wire(.jobCancelled))) }
        context.log.log(mount != nil ? BroMessage(.discMounted) : BroMessage(.discNotMounted, [("seconds", .integer(seconds))]))
        return StepReport(data: .object([("mountPath", mount.map { .string($0) } ?? .null)]))
    }
}
