import BroDomain
import BroFoundation
import BroPorts

/// Step 8 (plan §20.1, §12.4): makes what Plan decided in the job's staging folder (LibraryPort.stagingDirectory),
/// holding the drive, an acquisition slot and the library (LibraryWrite). It names, hashes and moves nothing.
/// - Space: the plan's need is reserved again (reservations don't outlive the engine), less what an earlier attempt kept.
/// - infoOnly: disc-info.json, the listing Probe read (Listing.encode; rip.discInfoWritten).
/// - mkv: the plan's titles from the source (and disc-info.json under archive.writeDiscInfo). One makemkvcon run for every
///   title of the listing ("all"; rip.ripping), or with Plan's one-pass minimum length when a listing read with it holds
///   exactly the titles (rip.onePassListing, then rip.onePass or rip.onePassMismatch); else title by title
///   (rip.rippingTitle), hand-picked tracks with +sel:all. A failed title (rip.titleFailed) leaves its files under
///   MakeMKV's names (rip.keptUnderMakemkvName) and the others are still ripped; the step then fails (rip.failed,
///   rip.titlesFailed) with the MakeMKV notice that explains it. A run that was stopped (drive.renumbered,
///   space.makemkvWarning, makemkv.stalled) fails the step with that reason at once.
/// - backup, backupDecrypted: the disc as a folder or an image (profile backup.format), named backup or backup.iso; an
///   image MakeMKV made for a folder gets .iso (backup.imageForFolder), a folder made for an image loses it
///   (backup.folderInsteadOfIso); backup.saved, or backup.failed (a stopped run: its reason; a folder without a disc
///   structure, which counts as nothing produced: backup.checkFailed).
/// - backupThenMkv: a decrypted backup, its structure checked (backup.checkFailed), then the titles from the backup
///   (mapped from Probe's listing to the backup's), and the backup removed unless backup.keepAfterMkv (backup.removed).
///
/// Read errors are counted per title (title by title) or per run (StepReport.readErrors); Release and Commit act on them.
/// A cancel removes what the stopped run wrote (rip.partialRemoved: makemkvcon leaves an unreadable file, P2-14) unless the
/// process was abandoned after KILL (process.abandoned {tool} is logged for any run whose process was: it may still be
/// writing, and JobService keeps the job's files where they are until a restart). After each title (its files synced) and after a checked backup, a checkpoint holds the
/// disc's fingerprint and what is finished: "backup" once a backup-then-MKV job has its backup (recovery resumes from it by
/// itself, plan §15.8: recovery.resumedFromBackup), else "titles". A later attempt of the step (a retry or a continue)
/// with the same disc keeps those titles (rip.continued) and removes anything else in staging (rip.leftoverRemoved; owner
/// 2026-10-18). Data: AcquireOutput.
public struct AcquireStep: Step {
    public static let titlesCheckpoint = "titles"
    public static let backupCheckpoint = "backup"
    public static let discInfoFile = "disc-info.json"

    public init() {}

    public var kind: StepKind { .acquire }

    public var resumability: Resumability { .resumeFrom }

    public var resumeCheckpoint: String? { AcquireStep.backupCheckpoint }

    public func resources(_ job: JobSnapshot) -> ResourceSet {
        var drives: [String] = []
        if case .drive(let id)? = DiscRequest.decode(job.request)?.source { drives = [id] }
        let library = PlanOutput.of(job).map(\.libraryId).flatMap { $0.isEmpty ? nil : [$0] } ?? []
        return ResourceSet(drives: drives, acquisition: true, libraryWrites: library)
    }

    public func run(_ context: StepContext) async throws(BroError) -> StepReport {
        try await Acquisition(context).run()
    }
}

/// One run of the step.
private final class Acquisition {
    private let c: StepContext
    private let plan: PlanOutput
    private let probe: ProbeOutput
    private let request: DiscRequest
    private let fs: any FileSystem
    private var staging = ""
    private var titles: [AcquiredTitle] = []
    private var others: [String] = []
    private var readErrors: [String] = []
    private var backup: String?
    private var discInfo: String?
    private var notice: MakemkvNotice?

    init(_ context: StepContext) throws(BroError) {
        c = context
        guard let plan = PlanOutput.of(context.job) else {
            throw BroError(MessageCode.wire(.internalUnexpected), [("detail", .string("acquire without a plan"))])
        }
        self.plan = plan
        probe = ProbeOutput.decode(JobSnapshot.output(context.job, .probe) ?? .null)
        request = try StepSources.request(context.job)
        fs = context.fs
    }

    func run() async throws(BroError) -> StepReport {
        let library = try c.library.require(plan.libraryId)
        staging = try c.library.stagingDirectory(plan.libraryId, jobId: c.job.id)

        // What an earlier attempt finished, with this disc.
        let saved = c.checkpoint.saved
        let same = saved?.members != nil && probe.fingerprint != nil && saved?["fingerprint"]?.string == probe.fingerprint
        let kind = saved?["kind"]?.string
        var resumeBackup: String?
        if same, plan.mode == .backupThenMkv, kind == AcquireStep.backupCheckpoint, let b = saved?["backup"]?.string, !b.isEmpty,
           fs.exists(staging + "/" + b) {
            resumeBackup = b
        }
        if same && ((plan.mode == .mkv && kind == AcquireStep.titlesCheckpoint)
            || (plan.mode == .backupThenMkv && (kind == AcquireStep.titlesCheckpoint || kind == AcquireStep.backupCheckpoint))) {
            for t in (saved?["titles"]?.array ?? []).map(AcquiredTitle.decode)
            where plan.titles.contains(where: { $0.index == t.index }) && !t.files.isEmpty && t.files.allSatisfy({ fs.exists(staging + "/" + $0) }) {
                titles.append(t)
            }
            readErrors += (saved?["readErrors"]?.array ?? []).map { $0.string ?? "" } // a backup's
        }
        let keep = Set(titles.flatMap(\.files) + [resumeBackup].compactMap { $0 })
        for e in try fs.list(staging).sorted(by: { $0.name.utf8.lexicographicallyPrecedes($1.name.utf8) }) where !keep.contains(e.name) {
            try FileTree.remove(fs, path: staging + "/" + e.name)
            c.log.log(BroMessage(.ripLeftoverRemoved, [("file", .string(e.name))]))
        }
        if resumeBackup != nil { c.log.log(BroMessage(.recoveryResumedFromBackup)) }
        if !titles.isEmpty { c.log.log(BroMessage(.ripContinued, [("count", .integer(Int64(titles.count)))])) }

        if let need = plan.spaceNeed {
            let kept = titles.reduce(Int64(0)) { sum, t in sum + (probe.listing?.titles.first { $0.index == t.index }?.sizeBytes ?? 0) }
            if need - kept > 0 {
                let volume = try fs.volume(library.path)
                try await c.space.reserve(volume.id, path: library.path, need: SpaceEstimate.required(Bytes(need - kept)).count, free: volume.freeBytes)
            }
        }

        switch plan.mode {
        case .infoOnly:
            guard let listing = probe.listing else { throw noInformation() }
            try writeDiscInfo(listing)
            return done()
        case .backup, .backupDecrypted:
            let made = try await makeBackup(decrypt: plan.mode == .backupDecrypted, from: 0, width: 1)
            if let stopped = made.report { return stopped }
            backup = made.name
            return done()
        case .mkv:
            guard let listing = probe.listing else { throw noInformation() }
            if profile("archive", "writeDiscInfo")?.bool == true { try writeDiscInfo(listing) }
            let source = try StepSources.makemkv(request.source, c.driveLookup)
            var map: [Int: Int] = [:]
            for t in plan.titles { map[t.index] = t.index }
            return try await rip(source, listing, map, AcquireStep.titlesCheckpoint, from: 0, width: 1) ?? done()
        case .backupThenMkv:
            guard let listing = probe.listing else { throw noInformation() }
            return try await backupThenMkv(listing, resumeBackup)
        default:
            throw BroError(MessageCode.wire(.internalUnexpected), [("detail", .string("mode " + plan.mode.rawValue))])
        }
    }

    private func backupThenMkv(_ listing: Listing, _ resumed: String?) async throws(BroError) -> StepReport {
        if plan.titles.allSatisfy({ p in titles.contains { $0.index == p.index } }) {
            backup = resumed
            return done()
        }
        var ripsFrom = 0.0
        let name: String
        if let resumed {
            name = resumed
        } else {
            let made = try await makeBackup(decrypt: true, from: 0, width: 0.5)
            if let stopped = made.report { return stopped }
            name = made.name!
            if let problem = BackupCheck.problem(fs, path: staging + "/" + name) {
                return failed(BroMessage(.backupCheckFailed, [("problem", problem.toJson())], severity: .error))
            }
            try FileTree.sync(fs, path: staging + "/" + name)
            backup = name
            try await save(AcquireStep.backupCheckpoint)
            ripsFrom = 0.5
        }
        backup = name
        let path = staging + "/" + name
        let source = SourceResolver.source(path, isDirectory: try fs.stat(path).isDirectory)
        let reading = BroMessage(.discOpReading)
        c.progress.report(ripsFrom, operation: reading, current: nil)
        let read = try await c.tools.makemkv.listing(source, invocation: try invocation(nil, nil), sink: sink(reading, ripsFrom, 0), cancel: c.cancel)
        logRun(read.run, command: "info")
        if read.run.outcome.status == .cancelled || c.cancel.isCancelled { return cancelled() }
        if read.listing.titles.isEmpty {
            let reason = read.run.outcome.error
                ?? BroMessage(.processExitStatus, [("status", .integer(Int64(read.run.outcome.exitCode)))], severity: .error)
            return failed(BroMessage(.discNoTitles, [("source", .string(path)), ("reason", reason.toJson())], severity: .error))
        }
        let handPicked = Set(plan.titles.filter { $0.tracks != nil }.map(\.index))
        let map = try ListingMatch.mapTitles(plan.titles.map(\.index), from: listing, to: read.listing, sameTracks: handPicked)
        if profile("archive", "writeDiscInfo")?.bool == true { try writeDiscInfo(read.listing) }
        if let stopped = try await rip(source, read.listing, map, AcquireStep.backupCheckpoint, from: ripsFrom, width: 1 - ripsFrom) { return stopped }
        if profile("backup", "keepAfterMkv")?.bool == false {
            backup = nil
            try await save(AcquireStep.titlesCheckpoint) // the titles stand without the backup from here on
            c.log.log(BroMessage(.backupRemoved, [("path", .string(path))]))
            try FileTree.remove(fs, path: path)
        }
        return done()
    }

    /// Rips the plan's titles that aren't done yet from `source`, whose listing is `listing` (`map`: the plan's index → its
    /// index there). Nil when every run succeeded; else the report the step ends with.
    private func rip(_ source: MakemkvSource, _ listing: Listing, _ map: [Int: Int], _ checkpoint: String, from: Double,
                     width: Double) async throws(BroError) -> StepReport? {
        let wanted = plan.titles.filter { p in !titles.contains { $0.index == p.index } }
        if wanted.isEmpty { return nil }
        let handPicked = wanted.contains { $0.tracks != nil }
        var runListing = listing
        var runMap = map
        var minLength: Int?
        if let m = plan.onePassMinLength, titles.isEmpty, !handPicked {
            let operation = BroMessage(.ripOnePassListing)
            c.progress.report(from, operation: operation, current: nil)
            let read = try await c.tools.makemkv.listing(source, invocation: try invocation(m, nil), sink: sink(operation, from, 0), cancel: c.cancel)
            logRun(read.run, command: "info")
            if read.run.outcome.status == .cancelled || c.cancel.isCancelled { return cancelled() }
            let indices = wanted.map { map[$0.index]! }
            if OnePass.matches(read.listing, chosen: indices, of: listing) {
                c.log.log(BroMessage(.ripOnePass, [("count", .integer(Int64(indices.count))), ("minLength", .integer(Int64(m))),
                                                   ("others", .integer(Int64(listing.titles.count - indices.count)))]))
                let pass = try ListingMatch.mapTitles(indices, from: listing, to: read.listing, sameTracks: [])
                for t in wanted { runMap[t.index] = pass[map[t.index]!] }
                runListing = read.listing
                minLength = m
            } else {
                c.log.log(BroMessage(.ripOnePassMismatch, [("minLength", .integer(Int64(m)))], severity: .warning))
            }
        }
        let single = !handPicked && wanted.count == runListing.titles.count
            && runListing.titles.allSatisfy { x in wanted.contains { runMap[$0.index] == x.index } }
        let runs: [[TitleChoice]] = single ? [wanted] : wanted.map { [$0] }
        var failures: [(title: Int?, reason: BroMessage)] = []
        for (n, group) in runs.enumerated() {
            if c.cancel.isCancelled { return cancelled() }
            let title = single ? "all" : String(runMap[group[0].index]!)
            let operation = single
                ? BroMessage(.ripRipping, [("count", .integer(Int64(group.count)))])
                : BroMessage(.ripRippingTitle, [("title", .integer(Int64(group[0].index))), ("n", .integer(Int64(n + 1))), ("of", .integer(Int64(runs.count)))])
            let at = from + width * Double(n) / Double(runs.count)
            c.progress.report(at, operation: operation, current: nil)
            let run = try await c.tools.makemkv.rip(source, title: title, destination: staging,
                                                    invocation: try invocation(minLength, !single && group[0].tracks != nil ? "+sel:all" : nil),
                                                    sink: sink(operation, at, width / Double(runs.count)), cancel: c.cancel)
            logRun(run, command: "mkv")
            let outcome = run.outcome
            if outcome.status == .cancelled || c.cancel.isCancelled {
                try removePartial(outcome.produced, abandoned: outcome.abandoned)
                return cancelled()
            }
            if outcome.status == .failed {
                let reason = outcome.error ?? BroMessage(.processExitStatus, [("status", .integer(Int64(outcome.exitCode)))], severity: .error)
                c.log.log(BroMessage(.ripTitleFailed, [("title", .string(single ? "all" : String(group[0].index))), ("reason", reason.toJson())],
                                     severity: .error))
                for f in outcome.produced {
                    c.log.log(BroMessage(.ripKeptUnderMakemkvName, [("file", .string(f))], severity: .warning))
                    others.append(f)
                }
                notice = notice ?? run.notice
                if Self.stops(reason) { return failed(reason) }
                failures.append((single ? nil : group[0].index, reason))
                continue
            }
            let errors = outcome.readErrors.map(\.text)
            var made: [AcquiredTitle] = []
            if single {
                for t in group {
                    let name = runListing.titles.first { $0.index == runMap[t.index] }!.outputFileName
                    made.append(AcquiredTitle(index: t.index, files: outcome.produced.filter { !name.isEmpty && $0 == name },
                                              readErrors: group.count == 1 ? errors : []))
                }
                others += outcome.produced.filter { f in !made.contains { $0.files.contains(f) } }
                if group.count > 1 { readErrors += errors }
            } else {
                made.append(AcquiredTitle(index: group[0].index, files: outcome.produced, readErrors: errors))
            }
            for f in outcome.produced { try fs.syncFile(staging + "/" + f) }
            titles += made
            try await save(checkpoint)
        }
        if failures.isEmpty { return nil }
        func reasonOf(_ f: (title: Int?, reason: BroMessage)) -> BroMessage {
            guard let t = f.title else { return f.reason }
            return BroMessage(.ripTitleReason, [("title", .string(String(t))), ("reason", f.reason.toJson())])
        }
        return failed(failures.count == 1
            ? BroMessage(.ripFailed, [("reason", reasonOf(failures[0]).toJson())], severity: .error)
            : BroMessage(.ripTitlesFailed, [("count", .integer(Int64(failures.count))), ("reasons", .array(failures.map { reasonOf($0).toJson() }))],
                         severity: .error))
    }

    /// A backup into staging: its name there, or the report the step ends with.
    private func makeBackup(decrypt: Bool, from: Double, width: Double) async throws(BroError) -> (name: String?, report: StepReport?) {
        let iso = profile("backup", "format")?.string == "iso"
        var name = iso ? "backup.iso" : "backup"
        let destination = staging + "/" + name
        let source = try StepSources.makemkv(request.source, c.driveLookup)
        let operation = BroMessage(.backupBackingUp, [("decrypted", .bool(decrypt))])
        c.progress.report(from, operation: operation, current: nil)
        let run = try await c.tools.makemkv.backup(source, decrypt: decrypt, destination: destination, invocation: try invocation(nil, nil),
                                                   sink: sink(operation, from, width), cancel: c.cancel)
        logRun(run, command: "backup")
        let outcome = run.outcome
        if outcome.status == .cancelled || c.cancel.isCancelled {
            if fs.exists(destination) { try removePartial([name], abandoned: outcome.abandoned) }
            return (nil, cancelled())
        }
        if outcome.status == .failed {
            notice = notice ?? run.notice
            let reason = outcome.error ?? BroMessage(.processExitStatus, [("status", .integer(Int64(outcome.exitCode)))], severity: .error)
            // A folder without a disc structure isn't a backup MakeMKV made (RunOutcome counts none): the check says why.
            if reason.code == .processSavedNothing && fs.exists(destination), (try? fs.stat(destination).isDirectory) == true,
               let problem = BackupCheck.problem(fs, path: destination) {
                return (nil, failed(BroMessage(.backupCheckFailed, [("problem", problem.toJson())], severity: .error)))
            }
            if fs.exists(destination) { others.append(name) }
            return (nil, failed(Self.stops(reason) ? reason : BroMessage(.backupFailed, [("reason", reason.toJson())], severity: .error)))
        }
        let folder = try fs.stat(destination).isDirectory
        if iso && folder {
            name = try rename(name, "backup", isFolder: true)
            c.log.log(BroMessage(.backupFolderInsteadOfIso, [("folder", .string(name))], severity: .warning))
        } else if !iso && !folder {
            name = try rename(name, "backup.iso", isFolder: false)
            c.log.log(BroMessage(.backupImageForFolder, [("file", .string(name))]))
        }
        c.log.log(BroMessage(.backupSaved, [("path", .string(staging + "/" + name))]))
        readErrors += outcome.readErrors.map(\.text)
        return (name, nil)
    }

    /// Renames a staging entry to a free name like `wanted`; the new name.
    private func rename(_ name: String, _ wanted: String, isFolder: Bool) throws(BroError) -> String {
        let free = ConflictNamer.next(wanted, existing: try fs.list(staging).map(\.name), isFolder: isFolder)
        try fs.rename(staging + "/" + name, to: staging + "/" + free)
        return free
    }

    /// What a stopped run wrote: unreadable (P2-14), so removed; kept when the process may still be writing.
    private func removePartial(_ names: [String], abandoned: Bool) throws(BroError) {
        if abandoned { return }
        for name in names {
            try FileTree.remove(fs, path: staging + "/" + name)
            c.log.log(BroMessage(.ripPartialRemoved, [("file", .string(name))]))
        }
    }

    private func writeDiscInfo(_ listing: Listing) throws(BroError) {
        try fs.writeAtomically(staging + "/" + AcquireStep.discInfoFile, bytes: JsonValue.encodeCanonical(Listing.encode(listing)), mode: 0o644)
        discInfo = AcquireStep.discInfoFile
        c.log.log(BroMessage(.ripDiscInfoWritten, [("file", .string(AcquireStep.discInfoFile))]))
    }

    /// The checkpoint: the disc, the titles done, the backup a backup-then-MKV job rips from and the read errors of runs no
    /// title owns (a backup's: its MKV files inherit them). The staging folder is synced first, so what the checkpoint names
    /// is there after a power loss.
    private func save(_ kind: String) async throws(BroError) {
        try fs.syncDirectory(staging)
        try await c.checkpoint.save(.object([("kind", .string(kind)), ("fingerprint", probe.fingerprint.map { .string($0) } ?? .null),
                                             ("titles", .array(titles.map(AcquiredTitle.encode))), ("backup", backup.map { .string($0) } ?? .null),
                                             ("readErrors", .array(readErrors.map { .string($0) }))]))
    }

    /// The plan's profile, with Plan's one-pass minimum length when given.
    private func invocation(_ minLength: Int?, _ selection: String?) throws(BroError) -> MakemkvInvocation {
        var profile = plan.profile
        if let m = minLength, let members = profile.members {
            let makemkv = (profile["makemkv"]?.members ?? []).filter { $0.key != "minLengthSeconds" } + [("minLengthSeconds", .integer(Int64(m)))]
            profile = .object(members.filter { $0.key != "makemkv" } + [("makemkv", .object(makemkv))])
        }
        return try c.makemkvSetup.invocation(c.config, profile: EffectiveProfile(profile: profile, steps: [], trace: []), selectionOverride: selection,
                                             jobDirectory: c.jobDirectory)
    }

    private func profile(_ section: String, _ key: String) -> JsonValue? { plan.profile[section]?[key] }

    private func sink(_ operation: BroMessage, _ from: Double, _ width: Double) -> MakemkvSink {
        MakemkvSink(progress: Portion(inner: c.progress, from: from, width: width), log: c.log, operation: operation)
    }

    private func logRun(_ run: MakemkvRun, command: String) {
        MakemkvSink.keepDebugLog(c, run.outcome, command: command)
        for case var problem? in [run.isolationWarning, run.transcriptProblem, run.settingsProblem] {
            problem.severity = .warning
            c.log.log(problem)
        }
        if run.outcome.abandoned { c.log.log(BroMessage(.processAbandoned, [("tool", .string("makemkvcon"))], severity: .error)) }
    }

    private func report(_ error: BroError?) -> StepReport {
        let order = plan.titles.map(\.index)
        let sorted = titles.sorted { (order.firstIndex(of: $0.index) ?? 0) < (order.firstIndex(of: $1.index) ?? 0) }
        let output = AcquireOutput(titles: sorted, others: others, backup: backup, discInfo: discInfo, readErrors: readErrors)
        return StepReport(data: AcquireOutput.encode(output), readErrors: AcquireOutput.readErrorCount(output), notice: error != nil ? notice : nil, error: error)
    }

    private func done() -> StepReport { report(nil) }

    private func failed(_ message: BroMessage) -> StepReport { report(message.toError()) }

    private func cancelled() -> StepReport { report(BroError(MessageCode.wire(.jobCancelled))) }

    /// A reason that stopped makemkvcon (RunOutcome: a renumbered drive, MakeMKV's space warning, a stall): the step fails
    /// with it, and no later title is ripped.
    private static func stops(_ reason: BroMessage) -> Bool {
        reason.code == .driveRenumbered || reason.code == .spaceMakemkvWarning || reason.code == .makemkvStalled
    }

    private func noInformation() -> BroError { BroMessage(.discNoInformation, severity: .error).toError() }
}

/// A run's progress as a part of the step's.
private struct Portion: ProgressReporter {
    let inner: any ProgressReporter
    let from: Double
    let width: Double

    func report(_ fraction: Double, operation: BroMessage, current: String?) {
        inner.report(from + width * min(max(fraction, 0), 1), operation: operation, current: current)
    }
}
