import BroDomain
import BroFoundation
import BroPorts

/// Step 12 (plan §20.1): decides the unit's folder in the library and every produced item's name in it, and renames
/// inside staging so staging is laid out exactly as the unit's folder will be (Seal hashes it so, Commit moves it).
/// - The names: Layouts (the profile's naming) with the identity's tokens, the date tokens in local time at the job's
///   start (Clock.localTime of JobSnapshot.startedAt, as today's apps use the rip's time); a title's hand-given name is its
///   own template. A backup is named by the file name template unless the job is backupThenMkv (the backup subfolder).
///   disc-info.json and the files no title owns keep their names.
/// - A folder backup's BDMV/index.bdmv (or VIDEO_TS) gives the disc's format when it differs from the identity's (a UHD
///   disc read without a listing looks like a Blu-ray; name.formatFromBackup): the names use it and NameOutput carries the
///   corrected identity for Seal's record.
/// - Nothing is overwritten (I5): a folder of its own takes a free name ("Name (2)"; conflictPolicy skip fails with
///   library.folderExists); a shared folder (media server layout, conflictPolicy merge) is merged item by item, so an item
///   whose name is taken there takes a free one. Names in staging never collide either, and an item never takes another
///   item's Acquire name.
/// - An item whose place holds a target (its own too, ASCII case ignored: Acquire's "backup" and a media server's
///   "Backup/") first goes to a free name at staging's root; an item inside another item's target fails with
///   name.unusable.
/// - A name that would hold no file (empty, a component starting with '.', one of Bromelia's own file names at the unit's
///   root) fails with name.unusable, an empty folder with name.noFolder, before anything is renamed.
/// Idempotent: the decisions (folder, and per item where it is and where it goes) are saved as the "names" checkpoint
/// before the first rename, so an attempt after a crash or a retry finds each item where it is and keeps the names
/// already given, unless one is now taken in the library; a checkpoint for other items than Acquire's (Acquire ran again) is
/// ignored. Names compare byte for byte (decision 79). Data: NameOutput.
/// Skipped when Plan or Acquire hasn't run.
public struct NameStep: Step {
    public static let namesCheckpoint = "names"

    public init() {}

    public var kind: StepKind { .name }

    public var resumability: Resumability { .idempotent }

    public func resources(_ job: JobSnapshot) -> ResourceSet {
        guard let plan = PlanOutput.of(job), !plan.libraryId.isEmpty else { return .none }
        return ResourceSet(libraryWrites: [plan.libraryId])
    }

    public func run(_ context: StepContext) async throws(BroError) -> StepReport {
        let job = context.job
        let fs = context.fs
        guard let plan = PlanOutput.of(job), let acquired = AcquireOutput.of(job) else { return StepReport(skipped: true) }
        let library = try context.library.require(plan.libraryId).path
        let staging = try context.library.stagingDirectory(plan.libraryId, jobId: job.id)
        var naming = NamingSettings.decode(plan.profile["naming"] ?? .object([]))
        if plan.mode != .backupThenMkv { naming.backupSubfolder = "" }
        let items = NameOutput.fromAcquire(acquired)
        var saved: JsonValue?
        if let s = context.checkpoint.saved, s.members != nil, s["kind"]?.string == NameStep.namesCheckpoint { saved = s }
        // A checkpoint made for other items than Acquire's is no guide: Acquire ran again since (a retry from it or Plan), and
        // removed whatever Name had renamed, so every item is under Acquire's name.
        if let s = saved, Set((s["files"]?.array ?? []).map { Array(($0["from"]?.string ?? "").utf8) }) != Set(items.map { Array($0.path.utf8) }) {
            saved = nil
        }
        var savedFiles: [[UInt8]: JsonValue] = [:]
        for f in saved?["files"]?.array ?? [] {
            if let from = f["from"]?.string, savedFiles[Array(from.utf8)] == nil { savedFiles[Array(from.utf8)] = f }
        }

        // 1. Where each produced item is now: where an earlier attempt sent it (its target, or the free name it went to on the
        // way), else where that attempt found it, else under Acquire's name. In that order: a folder made for a target may
        // be where an item was (ASCII case aside), but nothing but the item is ever at its target or its way.
        var at: [String] = []
        for item in items {
            let was = savedFiles[Array(item.path.utf8)]
            guard let found = [was?["to"]?.string, was?["via"]?.string, was?["at"]?.string, item.path].compactMap({ $0 })
                .first(where: { !$0.isEmpty && fs.exists(staging + "/" + $0) }) else {
                throw NameStep.failure(.archiveFileMissing, [("file", .string(item.path))])
            }
            at.append(found)
        }

        // 2. The format a folder backup shows.
        var identity = plan.identity
        var corrected: Identity?
        if let backup = items.firstIndex(where: { $0.role == "backup" }), try fs.stat(staging + "/" + at[backup]).isDirectory,
           let format = try NameStep.backupFormat(fs, staging + "/" + at[backup]), format != identity.format {
            identity.format = format
            identity.formatCode = FormatDetector.code(format, encrypted: identity.encrypted)
            corrected = identity
            context.log.log(BroMessage(.nameFormatFromBackup, [("format", .string(identity.formatCode.text))], severity: .info))
        }
        let probe = ProbeOutput.decode(JobSnapshot.output(job, .probe) ?? .null)
        let values = TokenRegistry.values(identity, context: try NameStep.tokens(context, plan, probe.listing))

        // 3. The unit's folder.
        let wanted = Layouts.folder(naming, values: values)
        if wanted.isEmpty { throw NameStep.failure(.nameNoFolder, []) }
        if NameStep.asciiLower(NameStep.first(wanted)) == "_quarantine" { throw NameStep.failure(.nameUnusable, [("path", .string(wanted))]) }
        let merge = naming.layout == .mediaServer || naming.conflictPolicy == .merge
        var folder = wanted
        if !merge {
            if let kept = saved?["folder"]?.string, !kept.isEmpty, try !NameStep.taken(fs, library, kept),
               naming.conflictPolicy != .skip || NameStep.same(kept, wanted) {
                folder = kept
            } else if try NameStep.taken(fs, library, wanted) {
                if naming.conflictPolicy == .skip { throw NameStep.failure(.libraryFolderExists, [("path", .string(library + "/" + wanted))]) }
                folder = NameStep.join(NameStep.parent(wanted), ConflictNamer.next(NameStep.last(wanted),
                                                                                  existing: try NameStep.names(fs, NameStep.join(library, NameStep.parent(wanted))),
                                                                                  isFolder: true))
            }
        }

        // 4. Every item's name in it: a name kept from an earlier attempt, else a free one: never a name another entry in
        // staging holds, nor another item's Acquire name (where a retry from Acquire puts that item again).
        func inStaging(_ i: Int, _ dir: String) throws(BroError) -> [String] {
            // The item's own location isn't in the way of its new name.
            let own = NameStep.same(NameStep.parent(at[i]), dir)
            return try NameStep.names(fs, NameStep.join(staging, dir)).filter { !(own && NameStep.same($0, NameStep.last(at[i]))) }
                + items.indices.filter { $0 != i && NameStep.same(NameStep.parent(items[$0].path), dir) }.map { NameStep.last(items[$0].path) }
        }
        let desired = try NameStep.desired(items, plan, acquired, probe.listing, naming, values, wanted)
        var to = [String?](repeating: nil, count: items.count)
        for (i, item) in items.enumerated() {
            if let kept = savedFiles[Array(item.path.utf8)]?["to"]?.string, !kept.isEmpty, try !(merge ? NameStep.taken(fs, library + "/" + folder, kept) : false) {
                to[i] = kept
            }
        }
        for i in items.indices where to[i] == nil {
            let dir = NameStep.parent(desired[i])
            var taken = try inStaging(i, dir)
            taken += to.compactMap { $0 }.filter { NameStep.same(NameStep.parent($0), dir) }.map(NameStep.last)
            if merge { taken += try NameStep.names(fs, NameStep.join(library + "/" + folder, dir)) }
            let free = NameStep.join(dir, ConflictNamer.next(NameStep.last(desired[i]), existing: taken,
                                                              isFolder: try fs.stat(staging + "/" + at[i]).isDirectory))
            // A case-insensitive disk can't rename to a case alone.
            to[i] = NameStep.same(NameStep.asciiLower(free), NameStep.asciiLower(at[i])) ? at[i] : free
        }
        let targets = to.map { $0! }
        // No item goes inside another item's target. An item whose place holds a target (its own too, ASCII case ignored: a
        // disk may not tell "backup" from "Backup/") first goes to a free name at staging's root, so nothing moves into a
        // folder that moves, nor into itself.
        for i in items.indices where items.indices.contains(where: { $0 != i && NameStep.inside(targets[i], targets[$0]) }) {
            throw NameStep.failure(.nameUnusable, [("path", .string(folder + "/" + targets[i]))])
        }
        var via = [String?](repeating: nil, count: items.count)
        var root = try NameStep.names(fs, staging) + items.indices.flatMap { [items[$0].path, at[$0], targets[$0]] }.map(NameStep.first)
        for j in items.indices where !NameStep.same(targets[j], at[j]) && targets.contains(where: { NameStep.inside($0, at[j]) }) {
            let free = ConflictNamer.next(NameStep.last(at[j]), existing: root, isFolder: try fs.stat(staging + "/" + at[j]).isDirectory)
            via[j] = free
            root.append(free)
        }

        // 5. The decisions are kept before anything moves, then the renames.
        let checkpoint = JsonValue.object([
            ("kind", .string(NameStep.namesCheckpoint)), ("folder", .string(folder)),
            ("files", .array(items.indices.map { i in
                .object([("from", .string(items[i].path)), ("at", .string(at[i])), ("to", .string(targets[i]))] + (via[i].map { [("via", .string($0))] } ?? []))
            })),
        ])
        try NameStep.cancelled(context)
        if saved == nil || JsonValue.encodeCanonical(saved!) != JsonValue.encodeCanonical(checkpoint) { try await context.checkpoint.save(checkpoint) }
        context.log.log(BroMessage(.archiveOutputFolder, [("path", .string(library + "/" + folder))], severity: .info))
        for i in items.indices { if let v = via[i] { try NameStep.rename(context, staging, at[i], v) } }
        for i in items.indices where !NameStep.same(targets[i], via[i] ?? at[i]) { try NameStep.rename(context, staging, via[i] ?? at[i], targets[i]) }
        var files = items
        for i in files.indices { files[i].path = targets[i] }
        return StepReport(data: NameOutput.encode(NameOutput(folder: folder, name: NameStep.last(folder), merge: merge, files: files,
                                                             identity: corrected)))
    }

    /// Renames `from` to `to` in staging, making the folders it goes in; a cancel comes first.
    private static func rename(_ context: StepContext, _ staging: String, _ from: String, _ to: String) throws(BroError) {
        try cancelled(context)
        var parent = staging
        for part in NameStep.parent(to).split(separator: "/") {
            parent += "/" + part
            if !context.fs.exists(parent) { try context.fs.createDirectory(parent, parentsMustExist: true) }
        }
        try context.fs.rename(staging + "/" + from, to: staging + "/" + to)
        context.log.log(BroMessage(.ripRenamed, [("from", .string(from)), ("to", .string(to))], severity: .info))
    }

    /// Each item's wanted path in the unit's folder (Layouts.paths without the folder), or name.unusable.
    private static func desired(_ items: [NamedFile], _ plan: PlanOutput, _ acquired: AcquireOutput, _ listing: Listing?, _ naming: NamingSettings,
                                _ values: [String: String], _ folder: String) throws(BroError) -> [String] {
        let titles = acquired.titles.map { t in listing?.titles.first { $0.index == t.index } }
        // The longest, the first on a tie.
        var main: Int?
        var longest = Int.min
        for case let t? in titles where t.durationSeconds > longest {
            longest = t.durationSeconds
            main = t.index
        }
        var outputs: [PlannedOutput?] = []
        var hand: [Int: String] = [:]
        for item in items {
            let ext = extensionOf(item.path)
            if item.role == "title", let index = item.title {
                let n = (acquired.titles.firstIndex { $0.index == index } ?? -1) + 1
                let t = n > 0 ? titles[n - 1] : nil
                let v: [String: String] = [
                    "track": t.map(TokenRegistry.trackLabel) ?? "",
                    "title": t.map { !$0.name.isEmpty ? $0.name : listing?.name ?? "" } ?? listing?.name ?? "",
                    "index": String(index), "n": String(n), "source": t?.sourceTitleId.map { String($0) } ?? "",
                    "duration": t.map { duration($0.durationSeconds) } ?? "",
                    "chapters": t.map { String($0.chapters) } ?? "", "original": stem(item.path), "comment": t?.comment ?? "",
                ]
                if let name = plan.titles.first(where: { $0.index == index }).map({ trimmed($0.name) }), !name.isEmpty {
                    var all = values
                    for (k, value) in v { all[k] = value }
                    let file = TemplateEngine.renderPath(name, values: all)
                    hand[outputs.count] = file.isEmpty || asciiLower(file).utf8.reversed().starts(with: ".mkv".utf8.reversed()) ? file : file + ".mkv"
                }
                outputs.append(PlannedOutput(role: .title, values: v, title: index, mainFeature: index == main, extension: ext))
            } else if item.role == "backup" {
                var v = Dictionary(uniqueKeysWithValues: ["title", "index", "n", "source", "duration", "chapters", "original", "comment"].map { ($0, "") })
                v["rip"] = "Backup"
                outputs.append(PlannedOutput(role: .backup, values: v, extension: ext))
            } else {
                outputs.append(nil)
            }
        }
        let paths = Layouts.paths(naming, values: values, outputs: outputs.compactMap { $0 })
        var result: [String] = []
        var p = 0
        for (i, item) in items.enumerated() {
            var rel: String
            if outputs[i] == nil {
                rel = item.path
            } else {
                let path = paths[p].path
                p += 1
                if let h = hand[i] {
                    rel = h
                } else if path.utf8.count > folder.utf8.count {
                    rel = String(decoding: path.utf8.dropFirst(folder.utf8.count).drop { $0 == UInt8(ascii: "/") }, as: UTF8.self)
                } else {
                    rel = ""
                }
            }
            let parts = rel.split(separator: "/", omittingEmptySubsequences: false)
            if parts.contains(where: { $0.isEmpty || $0.utf8.first == UInt8(ascii: ".") }) || (parts.count == 1 && ArchiveFiles.isOwnFile(rel)) {
                throw failure(.nameUnusable, [("path", .string(folder + "/" + rel))])
            }
            result.append(rel)
        }
        return result
    }

    /// What the tokens shared by every file need besides the identity.
    private static func tokens(_ context: StepContext, _ plan: PlanOutput, _ listing: Listing?) throws(BroError) -> TokenContext {
        let request = try StepSources.request(context.job)
        var drive: DriveLocation?
        if case .drive(let id) = request.source { drive = context.driveLookup.find(id) }
        let driveName = drive?.configId.flatMap { configId in Config.drives(context.config).first { $0.id == configId }?.name } ?? ""
        let label = StepSources.label(request.source, drive)
        let jobId = context.job.id.value.filter { $0 != "-" }
        return TokenContext(rip: plan.mode == .mkv || plan.mode == .backupThenMkv ? "Rip" : "Backup", drive: driveName,
                            disc: listing.map { !$0.name.isEmpty ? $0.name : label } ?? label,
                            volume: listing.map { !$0.volumeName.isEmpty ? $0.volumeName : label } ?? label,
                            type: listing?.type ?? .disc, job: String(jobId.prefix(8)), releaseYear: nil,
                            localTime: context.clock.localTime(context.job.startedAt ?? context.clock.now()))
    }

    /// The format a folder backup's structure shows (VIDEO_TS: DVD; BDMV/index.bdmv: UHD or Blu-ray), or nil.
    private static func backupFormat(_ fs: any FileSystem, _ backup: String) throws(BroError) -> DiscFormat? {
        let index = backup + "/BDMV/index.bdmv"
        // As ASCII: a byte above 127 is '?'.
        let text = fs.exists(index) ? String(decoding: try fs.readRange(index, offset: 0, length: 8).map { $0 < 128 ? $0 : UInt8(ascii: "?") }, as: UTF8.self) : nil
        let format = FormatDetector.detect(nil, flags: nil, indexBdmv: text, hasVideoTs: fs.exists(backup + "/VIDEO_TS"))
        return format == .unknown ? nil : format
    }

    /// Whether `relative` is taken under `root`, ignoring ASCII case as ConflictNamer does.
    private static func taken(_ fs: any FileSystem, _ root: String, _ relative: String) throws(BroError) -> Bool {
        !same(ConflictNamer.next(last(relative), existing: try names(fs, join(root, parent(relative)))), last(relative))
    }

    /// The names in a folder; none when it isn't there.
    private static func names(_ fs: any FileSystem, _ folder: String) throws(BroError) -> [String] {
        guard fs.exists(folder), try fs.stat(folder).isDirectory else { return [] }
        return try fs.list(folder).map(\.name)
    }

    /// Whether `path` lies inside the folder `folder`, ignoring ASCII case.
    private static func inside(_ path: String, _ folder: String) -> Bool {
        asciiLower(path).utf8.starts(with: Array(asciiLower(folder).utf8) + [UInt8(ascii: "/")])
    }

    /// Byte for byte: an NFC and an NFD spelling are two names on disk.
    private static func same(_ a: String, _ b: String) -> Bool { a.utf8.elementsEqual(b.utf8) }

    private static func parent(_ path: String) -> String {
        guard let i = path.utf8.lastIndex(of: UInt8(ascii: "/")) else { return "" }
        return String(path[..<i])
    }

    private static func last(_ path: String) -> String {
        guard let i = path.utf8.lastIndex(of: UInt8(ascii: "/")) else { return path }
        return String(path[path.utf8.index(after: i)...])
    }

    private static func first(_ path: String) -> String {
        guard let i = path.utf8.firstIndex(of: UInt8(ascii: "/")) else { return path }
        return String(path[..<i])
    }

    private static func join(_ a: String, _ b: String) -> String { a.isEmpty ? b : b.isEmpty ? a : a + "/" + b }

    /// From the last '.' after the first character of the last component; "" when there is none.
    private static func extensionOf(_ path: String) -> String {
        let name = last(path)
        guard let dot = name.utf8.lastIndex(of: UInt8(ascii: ".")), dot > name.startIndex else { return "" }
        return String(name[dot...])
    }

    private static func stem(_ path: String) -> String {
        let name = last(path)
        return String(decoding: name.utf8.dropLast(extensionOf(path).utf8.count), as: UTF8.self)
    }

    /// h-mm-ss.
    private static func duration(_ seconds: Int) -> String {
        func two(_ n: Int) -> String { n < 10 ? "0" + String(n) : String(n) }
        return String(seconds / 3600) + "-" + two(seconds / 60 % 60) + "-" + two(seconds % 60)
    }

    /// ASCII white space (space, \t, \n, \v, \f, \r) cut from both ends, as C's g_strstrip.
    private static func trimmed(_ s: String) -> String {
        func space(_ b: UInt8) -> Bool { b == 32 || (b >= 9 && b <= 13) }
        let bytes = Array(s.utf8)
        guard let start = bytes.firstIndex(where: { !space($0) }), let end = bytes.lastIndex(where: { !space($0) }) else { return "" }
        return String(decoding: bytes[start...end], as: UTF8.self)
    }

    /// Upper-case ASCII letters lowered, as ConflictNamer compares names.
    private static func asciiLower(_ s: String) -> String {
        String(String.UnicodeScalarView(s.unicodeScalars.map { $0.value >= 65 && $0.value <= 90 ? Unicode.Scalar($0.value + 32)! : $0 }))
    }

    private static func cancelled(_ context: StepContext) throws(BroError) {
        if context.cancel.isCancelled { throw BroError(MessageCode.wire(.jobCancelled)) }
    }

    private static func failure(_ code: MessageCode, _ params: [(key: String, value: JsonValue)]) -> BroError {
        BroMessage(code, params, severity: .error).toError()
    }
}
