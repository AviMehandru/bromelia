import BroDomain
import BroFoundation

/// Step 9 (plan §20.1): ejects the disc when the drive's automation.ejectWhenDone says so (drive.ejecting,
/// drive.ejected). A failed eject is logged as drive.ejectFailed (a warning) and doesn't fail the job. The runner then
/// gives back the drive and moves the job to the processing queue: the drive takes the next disc. Skipped when the job
/// isn't on a drive. Data: {ejected}.
public struct ReleaseStep: Step {
    public init() {}

    public var kind: StepKind { .release }

    public var resumability: Resumability { .idempotent }

    public func resources(_ job: JobSnapshot) -> ResourceSet { ResourceSet(drives: job.request.driveId.map { [$0] } ?? []) }

    public func run(_ context: StepContext) async throws(BroError) -> StepReport {
        guard case .drive(let driveId) = try StepSources.request(context.job).source else { return StepReport(skipped: true) }
        guard let at = context.driveLookup.find(driveId), !at.device.isEmpty,
              Config.automation(context.config, at.configId)["ejectWhenDone"]?.bool == true else { return ejected(false) }
        context.progress.report(0, operation: BroMessage(.driveEjecting), current: nil)
        do throws(BroError) {
            try await context.drives.eject(at.device)
        } catch {
            context.log.log(BroMessage(.driveEjectFailed, [("device", .string(at.device)), ("reason", .string(error.code))], severity: .warning))
            return ejected(false)
        }
        context.log.log(BroMessage(.driveEjected))
        return ejected(true)
    }

    private func ejected(_ value: Bool) -> StepReport { StepReport(data: .object([("ejected", .bool(value))])) }
}
