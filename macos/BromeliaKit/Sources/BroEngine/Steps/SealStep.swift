import BroDomain
import BroFoundation
import BroPorts

/// Step 13 (plan §20.1; §22.3 steps 1–3): makes the job's staging folder a sealed unit, so the commit only moves it.
/// What it seals is the produced items by name (review L3-1: the lists, never a count): Name's output, else Acquire's
/// names as they are. An item that isn't in staging, or holds no file (an empty folder, a hidden path, a path through
/// '..'; owner, 2026-10-21), fails with archive.fileMissing before anything is touched, a visible file no step made
/// with archive.unlisted (owner, 2026-10-20), and leftovers of a stopped write are removed. Then, in order:
/// 1. every file is synced to disk, then every folder from staging down to each file (archive.synced): a folder that
///    holds only folders (a backup's) is synced too, so each entry on the way to a file is durable;
/// 2. every file is hashed read back from disk (archive.hashFromDisk, on by default: the cache is bypassed), with
///    archive.hashing progress; a file that can't be read fails with archive.hashFailed;
/// 3. SHA256SUMS, the logs (the job's log.jsonl rendered line by line, MakeMKV's transcripts as they are), the READ ERRORS
///    note when MakeMKV reported read errors, and the record are written, each atomically; a write that fails fails with
///    archive.writeFailed. Bromelia's own files from an earlier attempt that this one doesn't write go.
/// SHA256SUMS and the record are always written; archive.checksums: false is ignored (archive.checksumsRequired) and
/// archive.archiveRecord: false leaves out only the logs (owner, 2026-10-20). Nothing is trusted from an earlier attempt:
/// a rerun syncs and hashes everything again and writes the same files. Read errors give the unit the tag "READ ERRORS"
/// (Commit puts it in _Quarantine/<name> [READ ERRORS], plan §22.2) and the record's status errors. Data: SealOutput.
/// Skipped when Plan or Acquire hasn't run.
public struct SealStep: Step {
    public static let readErrorsTag = "READ ERRORS"
    public static let readErrorsNote = "READ ERRORS.txt"
    /// The job folder's MakeMKV transcripts (MakemkvService's makemkv.txt; MakeMKV's debug log).
    public static let transcript = "makemkv.txt"
    public static let debugTranscript = "makemkv-debug.txt"
    private static let chunk = 1 << 20
    private static let fileMode = 0o644

    public init() {}

    public var kind: StepKind { .seal }

    public var resumability: Resumability { .idempotent }

    public func resources(_ job: JobSnapshot) -> ResourceSet {
        guard let plan = PlanOutput.of(job), !plan.libraryId.isEmpty else { return .none }
        return ResourceSet(libraryWrites: [plan.libraryId])
    }

    public func run(_ context: StepContext) async throws(BroError) -> StepReport { try SealStep.seal(context) }

    private static func seal(_ context: StepContext) throws(BroError) -> StepReport {
        let job = context.job
        let fs = context.fs
        guard let plan = PlanOutput.of(job), let acquired = AcquireOutput.of(job) else { return StepReport(skipped: true) }
        let staging = try context.library.stagingDirectory(plan.libraryId, jobId: job.id)
        let archive = plan.profile["archive"]
        let hashFromDisk = archive?["hashFromDisk"]?.bool ?? true
        let copyLogs = archive?["archiveRecord"]?.bool ?? true
        if archive?["checksums"]?.bool == false { context.log.log(BroMessage(.archiveChecksumsRequired, severity: .warning)) }

        // What is to be sealed, and nothing else.
        let named = NameOutput.of(job)?.files ?? NameOutput.fromAcquire(acquired)
        let walked = try FileTree.files(fs, root: staging)
        // An item that isn't there or holds no file (an empty folder, a hidden path, a path through '..') would seal
        // nothing for itself (owner, 2026-10-21): refused before anything is touched.
        for item in named where !ArchiveFiles.expand([item.path], tree: walked).contains(where: { !ArchiveFiles.isFolder($0) }) {
            throw failure(.archiveFileMissing, [("file", .string(item.path))])
        }
        var tree: [String] = []
        for f in walked {
            if !ArchiveFiles.isFolder(f), ArchiveFiles.isLeftover(ArchiveFiles.components(f).last ?? f) {
                try fs.remove(staging + "/" + f)
            } else {
                tree.append(f)
            }
        }
        let files = ArchiveFiles.expand(named.map(\.path), tree: tree).filter { !ArchiveFiles.isFolder($0) }
        // Names compare byte for byte (ArchiveFiles.expand): two spellings of one name are two files.
        let sealedFiles = Set(files.map { Array($0.utf8) })
        for f in tree {
            let hidden = ArchiveFiles.isHidden(f)
            let own = ArchiveFiles.isTop(f) && ArchiveFiles.isOwnFile(f)
            if !ArchiveFiles.isFolder(f), !hidden, !own, !sealedFiles.contains(Array(f.utf8)) { throw failure(.archiveUnlisted, [("file", .string(f))]) }
        }
        func itemOf(_ file: String) -> NamedFile {
            let bytes = Array(file.utf8)
            return named.first { n in
                let item = Array(trimmed(n.path).utf8)
                return bytes == item || bytes.starts(with: item + [UInt8(ascii: "/")])
            } ?? NamedFile(path: file, role: nil, title: nil)
        }

        // 1. Durable before it is read back.
        try cancelled(context)
        for f in files { try fs.syncFile(staging + "/" + f) }
        var folders: [[UInt8]: String] = [:]
        for f in files { for folder in foldersTo(f, from: staging) { folders[Array(folder.utf8)] = folder } }
        for (_, folder) in folders.sorted(by: { $0.key.lexicographicallyPrecedes($1.key) }) { try fs.syncDirectory(folder) }
        context.log.log(BroMessage(.archiveSynced, [("count", .integer(Int64(files.count)))]))

        // 2. Hashes of what is on disk.
        var sizes: [Int64] = []
        for f in files { sizes.append(try fs.stat(staging + "/" + f).size) }
        let total = max(1, sizes.reduce(0, +))
        var done: Int64 = 0
        var entries: [UnitFile] = []
        for (i, file) in files.enumerated() {
            try cancelled(context)
            context.progress.report(Double(done) / Double(total), operation: BroMessage(.archiveHashing), current: file)
            let (hash, size) = try hashOf(context, staging + "/" + file, file, fromDisk: hashFromDisk)
            let item = itemOf(file)
            entries.append(UnitFile(path: file, size: size, sha256: hash, role: item.role ?? "", title: item.title))
            done += sizes[i]
        }
        try cancelled(context)

        // 3. Bromelia's own files, each written atomically.
        let unitId = ArchiveFiles.unitId(job.id)
        let unit8 = String(unitId.value.prefix(8))
        var written: [String] = []
        func write(_ name: String, _ bytes: [UInt8]) throws(BroError) {
            try writeOwn(context, staging + "/" + name, bytes)
            written.append(name)
        }
        try write(Sha256Sums.fileName, Array(Sha256Sums.render(entries.map { SumEntry(path: $0.path, sha256: $0.sha256) }).utf8))
        context.log.log(BroMessage(.archiveChecksumsWritten, [("count", .integer(Int64(entries.count))), ("file", .string(Sha256Sums.fileName))]))

        let (log, logs) = try SealStep.copyLogs(context, unit8: unit8, copy: copyLogs, write: write)

        let readErrors = AcquireOutput.readErrorCount(acquired)
        var note: String?
        if readErrors > 0 {
            let errors = (acquired.titles.flatMap(\.readErrors) + acquired.readErrors).map { RobotMessage(code: 2003, flags: 0, count: 0, text: $0, format: $0, params: []) }
            let lines = Notes.render(job.id, outcome: .succeededWithReadErrors,
                                     error: BroMessage(.ripReadErrors, [("count", .integer(Int64(readErrors)))], severity: .error).toError(),
                                     readErrors: errors, logs: logs)
            try write(readErrorsNote, Array(lines.map { context.messages.render($0) + "\n" }.joined().utf8))
            note = readErrorsNote
        }

        let record = "bromelia-" + unit8 + ".json"
        let document = SealRecord.build(job, plan: plan, acquired: acquired, unitId: unitId, files: entries, log: log, logs: logs,
                                        finishedAt: context.clock.now(), appVersion: context.appVersion, messages: context.messages)
        try write(record, ArchiveRecordCodec.encodeV3(ArchiveRecord(version: 3, document: document, status: "", name: "", kind: "", label: "",
                                                                    volumeName: "", season: nil, part: nil, volume: nil, disc: nil, fingerprint: nil,
                                                                    unitId: nil, episodes: [], files: [])))
        context.log.log(BroMessage(.archiveRecordWritten, [("file", .string(record))]))

        // An earlier attempt's own files that this one didn't write (a note, a log no longer there).
        for f in tree where ArchiveFiles.isTop(f) && ArchiveFiles.isOwnFile(f) && !written.contains(where: { $0.utf8.elementsEqual(f.utf8) }) { try fs.remove(staging + "/" + f) }
        try fs.syncDirectory(staging)

        return StepReport(data: SealOutput.encode(SealOutput(unitId: unitId, status: readErrors > 0 ? "errors" : "success", files: entries, record: record,
                                                             logs: logs, note: note, tag: readErrors > 0 ? readErrorsTag : nil)))
    }

    /// The job's log (its lines, for the record) and, when `copy`, the copies written through `write`: the log as text
    /// (bromelia-<unit8>-log.txt) and MakeMKV's transcripts (makemkv-<unit8>-log.txt, makemkv-<unit8>-debug-log.txt), each
    /// when the job folder has it; their names. KeepApartStep copies them the same way.
    static func copyLogs(_ context: StepContext, unit8: String, copy: Bool,
                         write: (String, [UInt8]) throws(BroError) -> Void) throws(BroError) -> ([LogLineDto], [String]) {
        try copyLogs(context.fs, jobDirectory: context.jobDirectory, messages: context.messages, unit8: unit8, copy: copy, write: write)
    }

    /// copyLogs from the job folder `jobDirectory`, outside a step: RecoveryService's set-aside copies a failed or cancelled
    /// job's logs the same way (owner, 2026-10-25).
    static func copyLogs(_ fs: any FileSystem, jobDirectory: String, messages: any MessageRenderer, unit8: String, copy: Bool,
                         write: (String, [UInt8]) throws(BroError) -> Void) throws(BroError) -> ([LogLineDto], [String]) {
        let logFile = jobDirectory + "/" + JobLog.fileName
        let log = fs.exists(logFile) ? JobLog.decode(try fs.read(logFile)) : []
        var logs: [String] = []
        guard copy else { return (log, logs) }
        if fs.exists(logFile) {
            try write("bromelia-" + unit8 + "-log.txt", Array(logText(log, messages: messages).utf8))
            logs.append("bromelia-" + unit8 + "-log.txt")
        }
        for (from, to) in [(transcript, "makemkv-" + unit8 + "-log.txt"), (debugTranscript, "makemkv-" + unit8 + "-debug-log.txt")]
        where fs.exists(jobDirectory + "/" + from) {
            try write(to, try fs.read(jobDirectory + "/" + from))
            logs.append(to)
        }
        return (log, logs)
    }

    /// Writes one of Bromelia's own files atomically (0644); archive.writeFailed {path, reason} when it can't.
    static func writeOwn(_ context: StepContext, _ path: String, _ bytes: [UInt8]) throws(BroError) {
        do {
            try context.fs.writeAtomically(path, bytes: bytes, mode: fileMode)
        } catch {
            throw BroMessage(.archiveWriteFailed, [("path", .string(path)), ("reason", .string(text(context, error)))], severity: .error)
                .toError(cause: error)
        }
    }

    /// A file's SHA-256 and size, read through the FileSystem port (from disk when `fromDisk`); archive.hashFailed when it
    /// can't be read; job.cancelled between two pieces.
    static func hashOf(_ context: StepContext, _ path: String, _ file: String, fromDisk: Bool) throws(BroError) -> (String, Int64) {
        let stream: any ByteStream
        do {
            stream = try context.fs.openForReading(path, bypassCache: fromDisk)
        } catch {
            throw hashFailed(context, file, error)
        }
        defer { stream.close() }
        let hasher = Sha256Hasher()
        var size: Int64 = 0
        while true {
            try cancelled(context)
            let piece: [UInt8]
            do {
                piece = try stream.read(chunk)
            } catch {
                throw hashFailed(context, file, error)
            }
            if piece.isEmpty { break }
            hasher.update(piece)
            size += Int64(piece.count)
        }
        return (hasher.finish(), size)
    }

    /// The job's log as text: per line its time (UTC), [DEBUG] / [WARNING] / [ERROR] unless it is info, and the message in
    /// the configured language.
    public static func logText(_ lines: [LogLineDto], messages: any MessageRenderer) -> String {
        lines.map { l in
            Instant.format(l.t) + " " + (l.severity == .info ? "" : "[" + l.severity.rawValue.uppercased() + "] ")
                + messages.render(BroMessage(l.code, l.params, severity: l.severity)) + "\n"
        }.joined()
    }

    /// The folders from `staging` down to the one that holds `file`.
    static func foldersTo(_ file: String, from staging: String) -> [String] {
        var folders = [staging]
        for part in ArchiveFiles.components(file).dropLast() { folders.append(folders.last! + "/" + part) }
        return folders
    }

    private static func trimmed(_ path: String) -> String {
        var p = Array(path.utf8)
        while p.last == UInt8(ascii: "/") { p.removeLast() }
        return String(decoding: p, as: UTF8.self)
    }

    static func cancelled(_ context: StepContext) throws(BroError) {
        if context.cancel.isCancelled { throw BroError(MessageCode.wire(.jobCancelled)) }
    }

    private static func text(_ context: StepContext, _ error: BroError) -> String {
        context.messages.render(BroMessage(MessageCode.parse(error.code) ?? .internalUnexpected, error.params, severity: .error))
    }

    private static func hashFailed(_ context: StepContext, _ file: String, _ cause: BroError) -> BroError {
        BroMessage(.archiveHashFailed, [("file", .string(file)), ("reason", .string(text(context, cause)))], severity: .error).toError(cause: cause)
    }

    private static func failure(_ code: MessageCode, _ params: [(key: String, value: JsonValue)]) -> BroError {
        BroMessage(code, params, severity: .error).toError()
    }
}
