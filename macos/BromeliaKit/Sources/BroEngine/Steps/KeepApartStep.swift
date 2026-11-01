import BroDomain
import BroFoundation
import BroPorts

/// The step JobService.keepApart puts in place of the step an interrupted, failed or cancelled job stopped at and what
/// follows (owner, 2026-10-03 and 2026-10-25; plan §20.1, §20.4): the job isn't continued, and what it left in staging
/// becomes a sealed unit in <library>/_Quarantine/<name> [INCOMPLETE] with a quarantined row of status incomplete.
/// 1. Sealed as Seal seals, but with no produced lists (Acquire never finished): every visible file in staging (none hidden
///    by its first byte, not one of Bromelia's own at the top; leftovers of a stopped write removed) is synced, then each
///    folder down to it (archive.synced), and hashed read back from disk (archive.hashFromDisk) with archive.hashing
///    progress. SHA256SUMS, the logs as Seal copies them, INCOMPLETE.txt (the note recovery writes, with the logs, for the
///    outcome and error of the "outcome" checkpoint JobService stores with the step: interrupted without one) and the record
///    (status incomplete, no titles acquired) are written atomically; an earlier attempt's own files that this one doesn't
///    write go. Staging with no such file (a job that stopped before Acquire has an empty one) holds nothing to keep: its
///    empty folders are removed and no unit is made.
/// 2. The checkpoint "sealed" (the files and Bromelia's own files) is saved before the unit is handed to the library: a
///    rerun with it doesn't seal again (staging may already be moving) and hands over the same unit.
/// 3. library.quarantine(unit, intent, files, "INCOMPLETE"): the intent's items are staging's top-level entries that hold
///    the files (a file with its SHA-256, a folder without), then the record, the logs and the note; LibraryService picks
///    the free name, finishes an open intent and gives a unit quarantined already its result again, so a rerun rolls
///    forward. recovery.keptApart {path} (warning).
/// <name>: Plan's identity name, else the request's title, else the listing's name, else the source's label, else the job
/// id, the first that stays non-empty as a path component. Holds the library write; idempotent. Data: CommitOutput
/// (quarantined). Skipped without Plan's output. JobOutcome makes the job cancelled once the step succeeded. Paths compare
/// by their bytes.
public struct KeepApartStep: Step {
    public static let tag = "INCOMPLETE"
    public static let note = "INCOMPLETE.txt"
    public static let sealedCheckpoint = "sealed"
    public static let outcomeCheckpoint = "outcome"
    private static let quarantineFolder = "_Quarantine"

    public init() {}

    public var kind: StepKind { .keepApart }

    public var resumability: Resumability { .idempotent }

    public func resources(_ job: JobSnapshot) -> ResourceSet {
        guard let plan = PlanOutput.of(job), !plan.libraryId.isEmpty else { return .none }
        return ResourceSet(libraryWrites: [plan.libraryId])
    }

    /// The job's name in _Quarantine, before " [INCOMPLETE]" and ConflictNamer: the Plan output's identity name, else the
    /// job's title, else Probe's listing name, else the source's label, else the job's id (Sanitizer.component).
    /// RecoveryService sets a failed job's staging aside under it too (owner H3a-2).
    public static func folderName(_ job: JobSnapshot) -> String {
        let probe = JobSnapshot.output(job, .probe).flatMap { $0.members != nil ? ProbeOutput.decode($0) : nil }
        let source = DiscRequest.decode(job.request)?.source
        return [PlanOutput.of(job)?.identity.name ?? "", job.request.title, probe?.listing?.name ?? "", source.map { StepSources.label($0, nil) } ?? "",
                job.id.value]
            .map(Sanitizer.component).first { !$0.isEmpty } ?? job.id.value
    }

    public func run(_ context: StepContext) async throws(BroError) -> StepReport {
        let job = context.job
        guard let plan = PlanOutput.of(job) else { return StepReport(skipped: true) }
        let root = try context.library.require(plan.libraryId).path
        try SealStep.cancelled(context)
        let staging = try context.library.stagingDirectory(plan.libraryId, jobId: job.id)
        let unitId = ArchiveFiles.unitId(job.id)

        let files: [UnitFile]
        let own: [String]
        // The unit an earlier attempt sealed; one that names none of Bromelia's own files isn't (nothing writes one).
        if let saved = context.checkpoint.saved, saved["kind"]?.string == Self.sealedCheckpoint, !(saved["own"]?.array ?? []).isEmpty {
            (files, own) = Self.fromCheckpoint(saved)
        } else {
            (files, own) = try await Self.seal(context, plan, staging, unitId)
        }
        if files.isEmpty { return StepReport(data: .object([])) }

        let probe = JobSnapshot.output(job, .probe).flatMap { $0.members != nil ? ProbeOutput.decode($0) : nil }
        let source = DiscRequest.decode(job.request)?.source
        let sourceLabel = source.map { StepSources.label($0, nil) } ?? ""
        let label = if let listing = probe?.listing, !listing.name.isEmpty { listing.name } else { sourceLabel }
        let name = Self.folderName(job)
        let path = Self.quarantineFolder + "/" + name
        let now = context.clock.now()
        let identity = plan.identity
        let unit = UnitRecord(id: unitId, libraryId: plan.libraryId, physicalDiscId: nil, jobId: job.id, path: path, recordFile: own[0], recordVersion: 3,
                              state: .committing, status: .incomplete, name: identity.name, kind: identity.kind, format: identity.format.rawValue,
                              formatCode: identity.formatCode.text, encrypted: identity.encrypted, fingerprint: probe?.fingerprint, label: label,
                              season: identity.label.season, part: identity.label.part, volume: identity.label.volume, disc: identity.label.disc,
                              makemkvVersion: probe?.makemkvVersion ?? "", bytes: files.reduce(0) { $0 + $1.size }, fileCount: files.count, attempts: 1,
                              createdAt: now)
        let intent = CommitIntent(unitId: unitId, jobId: job.id, staging: staging, destination: root + "/" + path, merge: false, quarantine: false,
                                  createdAt: now, items: Self.items(files, own))

        context.progress.report(0, operation: BroMessage(.archiveMoving), current: nil)
        let result = try await context.library.quarantine(unit, intent: intent, files: files, tag: Self.tag, cancel: context.cancel)
        context.log.log(BroMessage(.recoveryKeptApart, [("path", .string(result.destination))], severity: .warning))
        let prefix = Array((root + "/").utf8)
        let kept = result.destination.utf8.starts(with: prefix)
            ? String(decoding: result.destination.utf8.dropFirst(prefix.count), as: UTF8.self) : result.destination
        return StepReport(data: CommitOutput.encode(CommitOutput(unitId: unitId, library: plan.libraryId, path: kept, merged: false, quarantined: true)))
    }

    /// The checkpoint JobService.keepApart stores with the step: the outcome the job had (interrupted, failed, cancelled) and
    /// its error, which the note names.
    public static func kept(_ outcome: Outcome, error: BroError?) -> JsonValue {
        var members: [(key: String, value: JsonValue)] = [("kind", .string(outcomeCheckpoint)), ("outcome", .string(outcome.rawValue))]
        if let error { members.append(("error", JsonDto.error(error))) }
        return .object(members)
    }

    /// Seals what staging holds; its files (none: nothing to keep, staging's empty folders removed) and Bromelia's own files
    /// (the record first), once the checkpoint that names them is stored.
    private static func seal(_ context: StepContext, _ plan: PlanOutput, _ staging: String, _ unitId: Id) async throws(BroError)
        -> ([UnitFile], [String]) {
        let fs = context.fs
        let archive = plan.profile["archive"]
        let hashFromDisk = archive?["hashFromDisk"]?.bool ?? true
        let copyLogs = archive?["archiveRecord"]?.bool ?? true
        var tree: [String] = []
        for f in try FileTree.files(fs, root: staging) {
            if !ArchiveFiles.isFolder(f), ArchiveFiles.isLeftover(ArchiveFiles.components(f).last ?? f) {
                try fs.remove(staging + "/" + f)
            } else {
                tree.append(f)
            }
        }
        let paths = tree.filter { !ArchiveFiles.isFolder($0) && !ArchiveFiles.isHidden($0) && !(ArchiveFiles.isTop($0) && ArchiveFiles.isOwnFile($0)) }
        if paths.isEmpty {
            try removeEmpty(fs, staging)
            return ([], [])
        }

        try SealStep.cancelled(context)
        for f in paths { try fs.syncFile(staging + "/" + f) }
        var folders: [[UInt8]: String] = [:]
        for f in paths { for folder in SealStep.foldersTo(f, from: staging) { folders[Array(folder.utf8)] = folder } }
        for (_, folder) in folders.sorted(by: { $0.key.lexicographicallyPrecedes($1.key) }) { try fs.syncDirectory(folder) }
        context.log.log(BroMessage(.archiveSynced, [("count", .integer(Int64(paths.count)))]))

        var sizes: [Int64] = []
        for f in paths { sizes.append(try fs.stat(staging + "/" + f).size) }
        let total = max(1, sizes.reduce(0, +))
        var done: Int64 = 0
        var files: [UnitFile] = []
        for (i, path) in paths.enumerated() {
            try SealStep.cancelled(context)
            context.progress.report(Double(done) / Double(total), operation: BroMessage(.archiveHashing), current: path)
            let (hash, size) = try SealStep.hashOf(context, staging + "/" + path, path, fromDisk: hashFromDisk)
            files.append(UnitFile(path: path, size: size, sha256: hash, role: ""))
            done += sizes[i]
        }
        try SealStep.cancelled(context)

        let unit8 = String(unitId.value.prefix(8))
        var written: [String] = []
        func write(_ name: String, _ bytes: [UInt8]) throws(BroError) {
            try SealStep.writeOwn(context, staging + "/" + name, bytes)
            written.append(name)
        }
        try write(Sha256Sums.fileName, Array(Sha256Sums.render(files.map { SumEntry(path: $0.path, sha256: $0.sha256) }).utf8))
        context.log.log(BroMessage(.archiveChecksumsWritten, [("count", .integer(Int64(files.count))), ("file", .string(Sha256Sums.fileName))]))
        let (log, logs) = try SealStep.copyLogs(context, unit8: unit8, copy: copyLogs, write: write)
        let kept = context.checkpoint.saved.flatMap { $0["kind"]?.string == outcomeCheckpoint ? $0 : nil }
        var error: BroError?
        if let e = kept?["error"], e.members != nil, let code = e["code"]?.string { error = BroError(code, e["params"]?.members ?? []) }
        let lines = Notes.render(context.job.id, outcome: kept?["outcome"]?.string.flatMap(Outcome.init(rawValue:)) ?? .interrupted, error: error,
                                 readErrors: [], logs: logs, fullLog: context.jobDirectory + "/" + JobLog.fileName)
        try write(note, Array(lines.map { context.messages.render($0) + "\n" }.joined().utf8))
        let record = "bromelia-" + unit8 + ".json"
        let acquired = AcquireOutput(titles: [], others: [], backup: nil, discInfo: nil, readErrors: [])
        let document = SealRecord.build(context.job, plan: plan, acquired: acquired, unitId: unitId, files: files, log: log, logs: logs,
                                        finishedAt: context.clock.now(), appVersion: context.appVersion, messages: context.messages, status: "incomplete")
        try write(record, ArchiveRecordCodec.encodeV3(ArchiveRecord(version: 3, document: document, status: "", name: "", kind: "", label: "",
                                                                    volumeName: "", season: nil, part: nil, volume: nil, disc: nil, fingerprint: nil,
                                                                    unitId: nil, episodes: [], files: [])))
        context.log.log(BroMessage(.archiveRecordWritten, [("file", .string(record))]))
        for f in tree where ArchiveFiles.isTop(f) && ArchiveFiles.isOwnFile(f) && !written.contains(where: { $0.utf8.elementsEqual(f.utf8) }) {
            try fs.remove(staging + "/" + f)
        }
        try fs.syncDirectory(staging)

        let own = [record] + logs + [note]
        try await context.checkpoint.save(.object([
            ("kind", .string(sealedCheckpoint)),
            ("files", .array(files.map { .object([("path", .string($0.path)), ("size", .integer($0.size)), ("sha256", .string($0.sha256))]) })),
            ("own", .array(own.map { .string($0) })),
        ]))
        return (files, own)
    }

    private static func fromCheckpoint(_ saved: JsonValue) -> ([UnitFile], [String]) {
        let files = (saved["files"]?.array ?? []).map {
            UnitFile(path: $0["path"]?.string ?? "", size: $0["size"]?.int ?? 0, sha256: $0["sha256"]?.string ?? "", role: "")
        }
        return (files, (saved["own"]?.array ?? []).map { $0.string ?? "" })
    }

    /// Staging's top-level entries that hold the files, in byte order (a file with its SHA-256, a folder without), then
    /// Bromelia's own files; each in place.
    private static func items(_ files: [UnitFile], _ own: [String]) -> [CommitItem] {
        var tops: [[UInt8]: String] = [:]
        for f in files { let top = ArchiveFiles.components(f.path)[0]; tops[Array(top.utf8)] = top }
        var items: [CommitItem] = []
        for (bytes, top) in tops.sorted(by: { $0.key.lexicographicallyPrecedes($1.key) }) {
            let file = files.first { $0.path.utf8.elementsEqual(bytes) }
            items.append(CommitItem(seq: items.count, fromPath: top, toPath: top, sha256: file?.sha256, isDir: file == nil, moved: false))
        }
        for o in own { items.append(CommitItem(seq: items.count, fromPath: o, toPath: o, sha256: nil, isDir: false, moved: false)) }
        return items
    }

    /// Removes the folder's empty folders, and the folder itself once empty; a file stays.
    private static func removeEmpty(_ fs: any FileSystem, _ folder: String) throws(BroError) {
        for e in try fs.list(folder) where e.isDirectory { try removeEmpty(fs, folder + "/" + e.name) }
        if try fs.list(folder).isEmpty { try fs.remove(folder) }
    }
}
