import BroDomain
import BroFoundation

/// Step 2 (plan §20.1): reads the disc's listing with makemkvcon (disc.op.reading), under the profile the drive gets
/// before Plan. A disc opened in a session is read again (disc.readingAgain): ReconcileStep checks it is still that
/// disc. A listing without titles fails with disc.noTitles and MakeMKV's notice (an expired key, LibreDrive required),
/// except for a backup, which goes on as today with the opened listing (disc.usingOpenedListing) or without one
/// (disc.continuingWithoutListing). Logs disc.found, the LibreDrive detail and what the run's isolation or transcript
/// reported (the isolation's warning, the transcript's problem, then what giving the settings back reported). Data:
/// ProbeOutput.
public struct ProbeStep: Step {
    public init() {}

    public var kind: StepKind { .probe }

    public var resumability: Resumability { .idempotent }

    public func resources(_ job: JobSnapshot) -> ResourceSet {
        if case .drive(let id)? = DiscRequest.decode(job.request)?.source { return ResourceSet(drives: [id]) }
        return .none
    }

    public func run(_ context: StepContext) async throws(BroError) -> StepReport {
        let request = try StepSources.request(context.job)
        let source = try StepSources.makemkv(request.source, context.driveLookup)
        var configId: String?
        if case .drive(let id) = request.source { configId = context.driveLookup.find(id)?.configId }
        let profile = StepSources.profile(context, configId)
        let operation = BroMessage(.discOpReading)
        if request.session != nil { context.log.log(BroMessage(.discReadingAgain)) }
        context.progress.report(0, operation: operation, current: nil)
        let invocation = try context.makemkvSetup.invocation(context.config, profile: profile, selectionOverride: nil, jobDirectory: context.jobDirectory)
        let read = try await context.tools.makemkv.listing(source, invocation: invocation,
                                                           sink: MakemkvSink(progress: context.progress, log: context.log, operation: operation),
                                                           cancel: context.cancel)
        let run = read.run
        MakemkvSink.keepDebugLog(context, run.outcome, command: "info")
        for case var problem? in [run.isolationWarning, run.transcriptProblem, run.settingsProblem] {
            problem.severity = .warning
            context.log.log(problem)
        }
        if run.outcome.status == .cancelled || context.cancel.isCancelled { return StepReport(error: BroError(MessageCode.wire(.jobCancelled))) }
        if let detail = run.libreDrive { context.log.log(BroMessage(.makemkvLibreDrive, [("detail", .string(detail))])) }

        let listing = read.listing
        if listing.titles.isEmpty {
            let reason = run.outcome.error ?? BroMessage(.processExitStatus, [("status", .integer(Int64(run.outcome.exitCode)))], severity: .error)
            let noTitles = BroMessage(.discNoTitles, [("source", .string(describe(source))), ("reason", reason.toJson())], severity: .error)
            let mode = StepSources.baseMode(context.job, profile)
            if mode != .backup && mode != .backupDecrypted { return StepReport(notice: run.notice, error: noTitles.toError()) }
            if let session = request.session {
                context.log.log(BroMessage(.discUsingOpenedListing, [("reason", noTitles.toJson())], severity: .warning))
                return done(ProbeOutput(listing: session.listing, fingerprint: Fingerprint.of(session.listing), libreDrive: run.libreDrive, fromSession: true,
                                       makemkvVersion: run.version))
            }
            context.log.log(BroMessage(.discContinuingWithoutListing, [("reason", noTitles.toJson())], severity: .warning))
            return done(ProbeOutput(listing: nil, fingerprint: nil, libreDrive: run.libreDrive, fromSession: false, makemkvVersion: run.version))
        }
        context.log.log(BroMessage(.discFound, [("count", .integer(Int64(listing.titles.count))), ("name", .string(listing.name))]))
        return done(ProbeOutput(listing: listing, fingerprint: Fingerprint.of(listing), libreDrive: run.libreDrive, fromSession: false,
                                makemkvVersion: run.version))
    }

    private func done(_ output: ProbeOutput) -> StepReport { StepReport(data: ProbeOutput.encode(output)) }

    /// The source as makemkvcon names it: disc:N, iso:path or file:path.
    private func describe(_ source: MakemkvSource) -> String {
        switch source {
        case .drive(let index, _): return "disc:\(index)"
        case .iso(let path): return "iso:" + path
        case .file(let path): return "file:" + path
        }
    }
}
