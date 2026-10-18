import BroDomain
import BroFoundation

/// Step 5 (plan §20.1), its phase 3 form: Identity.resolve from the listing Probe read (or none), the disc's label, the
/// drive's flags and the name and kind chosen in the session; a backup that stays encrypted (mode backup before Plan)
/// gets the e format code. Online lookups, the archive's previous disc and menu numbers join in phase 5. Logs
/// identity.resolved. Data: the Identity.
public struct IdentifyStep: Step {
    public init() {}

    public var kind: StepKind { .identify }

    public var resumability: Resumability { .idempotent }

    public func resources(_ job: JobSnapshot) -> ResourceSet { .none }

    public func run(_ context: StepContext) async throws(BroError) -> StepReport {
        let request = try StepSources.request(context.job)
        var drive: DriveLocation?
        if case .drive(let id) = request.source { drive = context.driveLookup.find(id) }
        let mode = StepSources.baseMode(context.job, StepSources.profile(context, drive?.configId))
        let identity = try IdentifyStep.resolve(context, encrypted: mode == .backup)
        let set = LabelParser.setDescription(identity.label)
        context.log.log(BroMessage(.identityResolved, [
            ("kind", .string(identity.kind.rawValue)), ("name", .string(identity.name)), ("hasSet", .bool(!set.isEmpty)), ("set", .string(set)),
            ("format", .string(identity.format.rawValue)), ("formatCode", .string(identity.formatCode.text)), ("reason", identity.reason.toJson()),
        ]))
        return StepReport(data: Identity.encode(identity))
    }

    /// The identity of the job's disc from Probe's listing (or none), the disc's label (the listing's name, else
    /// StepSources.label), the drive's flags and the session's name and kind; `encrypted`: a backup that stays encrypted.
    /// Plan resolves it again under the mode it chose.
    public static func resolve(_ context: StepContext, encrypted: Bool) throws(BroError) -> Identity {
        let request = try StepSources.request(context.job)
        let probe = ProbeOutput.decode(JobSnapshot.output(context.job, .probe) ?? .null)
        var drive: DriveLocation?
        if case .drive(let id) = request.source { drive = context.driveLookup.find(id) }
        let label = probe.listing.map(\.name).flatMap { $0.isEmpty ? nil : $0 } ?? StepSources.label(request.source, drive)
        let choices = request.session?.choices
        return Identity.resolve(IdentityInputs(listing: probe.listing, discLabel: label, encrypted: encrypted, flags: drive?.flags,
                                               nameOverride: choices?.name ?? "", kindOverride: choices?.kind))
    }
}
