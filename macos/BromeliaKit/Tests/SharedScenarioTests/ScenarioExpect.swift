import BroAdapters
import BroDomain
import BroEngine
import BroFoundation
import BroPorts
import BroTestSupport
import Foundation
import SQLite3

/// The expectations of an `expect` step (decisions 238, 241, 243), one function per key, each reading what the table of
/// decision 238 gives. Without `when` the step first settles (decision 239); with it, it waits for the condition and never
/// moves the clock. Placeholders: {root}, {unit8} and {jobId} of the job the step names (else the only job),
/// {unit8:<name>}, {jobId:<name>}, {driveId:<drive>}.
enum ScenarioExpect {
    private static let english = MessageTextRenderer(EnglishMessages.catalog)

    static func check(_ run: ScenarioRun, _ expect: JsonValue) async throws {
        if let when = expect["when"] { try await self.when(run, when) } else { try await run.settle() }
        let named = expect["job"]?["name"]?.string
        for (key, want) in expect.members ?? [] {
            switch key {
            case "when", "note":
                break
            case "job":
                try await expectJob(run, try await fill(run, want, named))
            case "jobs":
                try await expectJobs(run, want)
            case "files":
                try expectFiles(run, try await fill(run, want, named))
            case "file":
                try expectFile(run, try await fill(run, want, named))
            case "sums":
                try expectSums(run, try await fill(run, want, named))
            case "record":
                try await expectRecord(run, want, named)
            case "transcript":
                try expectTranscript(run, try await fill(run, want, named), named)
            case "debugLog":
                try expectDebugLog(run, try await fill(run, want, named), named)
            case "calls":
                try expectCalls(run, try await fill(run, want, named))
            case "log":
                try await expectLog(run, try await fill(run, want, named), named)
            case "events":
                try expectEvents(run, try await fill(run, want, named))
            case "problems":
                try await expectProblems(run, try await fill(run, want, named))
            case "drives":
                try await expectDrives(run, try await fill(run, want, named))
            case "db":
                try expectDb(run, try await fill(run, want, named))
            case "config":
                try await expectConfig(run, try await fill(run, want, named))
            case "manifest", "notifications":
                throw ScenarioFailure("\(key) (phase 5)", "not built")
            case "session", "power", "response":
                throw ScenarioFailure("\(key) (phase 4)", "not built")
            default:
                throw ScenarioFailure("a known expectation", key)
            }
        }
    }

    // MARK: When

    /// Waits (never moving the clock) until the job's step has started, the job is in the state (finished: it has an
    /// outcome) or has the outcome, or an event of the type has been published.
    static func when(_ run: ScenarioRun, _ when: JsonValue) async throws {
        let deadline = Date().addingTimeInterval(ScenarioRun.patience)
        while true {
            if try await holds(run, when) { return }
            if Date() > deadline { throw ScenarioFailure("when " + ScenarioRun.jsonText(when), "it never held within 10 s") }
            try await Task.sleep(nanoseconds: 5_000_000)
        }
    }

    private static func holds(_ run: ScenarioRun, _ when: JsonValue) async throws -> Bool {
        if let type = when["event"]?.string { return run.allEvents().contains { $0.type.rawValue == type } }
        let detail = try await run.root.engine.job(try run.job(when["job"]?.string ?? ""))
        if let step = when["step"]?.string, !detail.steps.contains(where: { $0.kind.rawValue == step && $0.startedAt != nil }) { return false }
        if let state = when["state"]?.string, detail.job.state.rawValue != state { return false }
        if let outcome = when["outcome"]?.string, detail.job.outcome?.rawValue != outcome { return false }
        return true
    }

    // MARK: Job, jobs

    static func expectJob(_ run: ScenarioRun, _ want: JsonValue) async throws {
        let name = try want["name"]?.string ?? onlyJob(run)
        let id = try run.job(name)
        let detail = try await run.root.engine.job(id)
        do {
            try await expectJob(run, want, name, id, detail)
        } catch let f as ScenarioFailure {
            let log = try await run.root.engine.jobLog(id, after: 0).map { l in
                l.code.rawValue + (l.params.isEmpty ? "" : " " + ScenarioRun.jsonText(.object(l.params)))
            }
            throw ScenarioFailure(f.message + "\n  (job \(name): \(detail.job.state.rawValue), error \(ScenarioRun.jsonText(JobDetailDto.json(detail)["error"])), steps "
                                  + detail.steps.map { $0.kind.rawValue + ":" + $0.state.rawValue }.joined(separator: " ") + ", log:\n    "
                                  + log.joined(separator: "\n    ") + ")")
        }
    }

    private static func expectJob(_ run: ScenarioRun, _ want: JsonValue, _ name: String, _ id: Id, _ detail: JobDetailDto) async throws {
        let json = JobDetailDto.json(detail)
        let what = "job " + name
        for (key, value) in want.members ?? [] {
            switch key {
            case "name":
                break
            case "state", "outcome", "mode", "kind", "queue", "automatic", "error", "blockedBy", "decision":
                try same(value, json[key], "\(what) \(key)")
            case "noError":
                if value.bool == true, let error = json["error"], !error.isNull {
                    throw ScenarioFailure("\(what) without an error", ScenarioRun.jsonText(error))
                }
            case "errorText":
                let text = json["error"].flatMap { $0.isNull ? nil : english.renderJson($0) } ?? ""
                let wanted = value.string ?? ""
                if !contains(text, wanted) { throw ScenarioFailure("\(what) error text containing \"\(wanted)\"", "\"\(text)\"") }
            case "parent":
                try same(.string(try run.job(value.string ?? "").value), json["parentId"], "\(what) parent")
            case "steps":
                try same(value, .array(detail.steps.filter { $0.state != .pending }.map { .string($0.kind.rawValue) }), "\(what) steps")
            case "files":
                try same(value, .integer(Int64(detail.files.count)), "\(what) files")
            case "titles":
                let store = run.root.store
                let rows = try await store.steps().load(id)
                let acquire = rows.last { $0.kind == .acquire && $0.output != nil }
                let titles = acquire.map { AcquireOutput.decode($0.output!.data).titles.map { JsonValue.integer(Int64($0.index)) } } ?? []
                try same(value, .array(titles), "\(what) titles")
            case "unit":
                let unit = detail.job.unitId ?? ArchiveFiles.unitId(id)
                let rows = try db(run, "archive_units").filter { $0["id"]?.string == unit.value }
                if (value.members ?? []).isEmpty { break }
                guard let row = rows.first else {
                    throw ScenarioFailure("\(what) unit \(ScenarioRun.jsonText(value))", "no archive_units row for " + unit.value)
                }
                try same(value, row, "\(what) unit")
            case "counts":
                let last = run.allEvents().last { ($0.type == .jobChanged || $0.type == .jobFinished) && $0.data["job"]?["id"]?.string == id.value }
                try same(value, last?.data["job"]?["counts"] ?? .null, "\(what) counts (its last jobChanged or jobFinished)")
            default:
                throw ScenarioFailure("a known job expectation", key)
            }
        }
    }

    private static func onlyJob(_ run: ScenarioRun) throws -> String {
        guard run.jobNames.count == 1 else { throw ScenarioFailure("one job, or a name", "\(run.jobNames.count) jobs") }
        return run.jobNames[0]
    }

    static func expectJobs(_ run: ScenarioRun, _ want: JsonValue) async throws {
        let jobs = try await run.allJobs()
        if let count = want["count"]?.int { try same(.integer(count), .integer(Int64(jobs.count)), "jobs count") }
        if let outcomes = want["outcomes"] { try same(outcomes, .array(jobs.map { $0.outcome.map { .string($0.rawValue) } ?? .null }), "jobs outcomes") }
        if let states = want["states"] { try same(states, .array(jobs.map { .string($0.state.rawValue) }), "jobs states") }
        if let kinds = want["kinds"] { try same(kinds, .array(jobs.map { .string($0.kind.rawValue) }), "jobs kinds") }
    }

    // MARK: Files

    /// Every file and folder under the folder, as paths relative to it (folders without a final '/').
    private static func walk(_ run: ScenarioRun, _ folder: String) throws -> (files: [String], folders: [String]) {
        var files: [String] = [], folders: [String] = []
        func visit(_ dir: String, _ rel: String) throws {
            for e in try run.root.fs.list(dir) {
                let r = rel.isEmpty ? e.name : rel + "/" + e.name
                if e.isDirectory {
                    folders.append(r)
                    try visit(dir + "/" + e.name, r)
                } else {
                    files.append(r)
                }
            }
        }
        if run.root.fs.exists(folder) { try visit(folder, "") }
        return (files, folders)
    }

    private static func hidden(_ path: String) -> Bool { path.split(separator: "/").contains { $0.hasPrefix(".") } }

    private static func sortedBytes(_ items: [String]) -> [String] { items.sorted { $0.utf8.lexicographicallyPrecedes($1.utf8) } }

    static func expectFiles(_ run: ScenarioRun, _ want: JsonValue) throws {
        let under = ScenarioRun.path(want["under"]?.string ?? "")
        let (files, folders) = try walk(run, ScenarioRun.trimSlash(under))
        let all = files + folders
        let whereText = want["under"]?.string.map { " under " + $0 } ?? ""
        if let exactly = want["exactly"]?.array {
            // The non-hidden files, and every non-hidden empty folder as name/ (a job that saved nothing leaves no folder).
            let listed = sortedBytes(files.filter { !hidden($0) }
                + folders.filter { d in !hidden(d) && !all.contains { $0.utf8.starts(with: (d + "/").utf8) } }.map { $0 + "/" })
            let patterns = exactly.compactMap(\.string)
            let ok = listed.allSatisfy { f in patterns.contains { glob($0, f) } } && patterns.allSatisfy { p in listed.contains { glob(p, $0) } }
            if !ok { throw ScenarioFailure("exactly \(show(sortedBytes(patterns)))\(whereText)", show(listed)) }
        }
        for p in (want["include"]?.array ?? []).compactMap(\.string) where !all.contains(where: { glob(p, $0) }) {
            throw ScenarioFailure("\(p)\(whereText)", show(sortedBytes(all.filter { !hidden($0) || p.contains(".") })))
        }
        for p in (want["exclude"]?.array ?? []).compactMap(\.string) {
            let found = all.filter { glob(p, $0) }
            if !found.isEmpty { throw ScenarioFailure("no \(p)\(whereText)", show(found)) }
        }
    }

    private static func show(_ items: [String]) -> String { "[" + items.map { "\"" + $0 + "\"" }.joined(separator: ", ") + "]" }

    /// A path pattern: * within one component, ** across components; a component starting with '.' is matched only by a
    /// pattern component that writes the dot.
    static func glob(_ pattern: String, _ path: String) -> Bool {
        let pats = ScenarioRun.trimSlash(pattern).split(separator: "/", omittingEmptySubsequences: false).map(String.init)
        let parts = ScenarioRun.trimSlash(path).split(separator: "/", omittingEmptySubsequences: false).map(String.init)
        return globParts(pats, 0, parts, 0) && (!pattern.hasSuffix("/") || path.hasSuffix("/"))
    }

    private static func globParts(_ pats: [String], _ i: Int, _ parts: [String], _ j: Int) -> Bool {
        if i == pats.count { return j == parts.count }
        if pats[i] == "**" { return (j...parts.count).contains { globParts(pats, i + 1, parts, $0) } }
        if j == parts.count { return false }
        if parts[j].hasPrefix(".") && !pats[i].hasPrefix(".") { return false }
        return ScenarioTools.glob(pats[i], parts[j]) && globParts(pats, i + 1, parts, j + 1)
    }

    /// Whether `text` holds `part`, byte for byte (Swift's String.contains compares graphemes).
    private static func contains(_ text: String, _ part: String) -> Bool { find(text, part, from: 0) != nil }

    /// The byte offset of `part` in `text` at or after `from`.
    private static func find(_ text: String, _ part: String, from: Int) -> Int? {
        let t = Array(text.utf8), p = Array(part.utf8)
        if p.isEmpty { return from <= t.count ? from : nil }
        if t.count < p.count { return nil }
        var i = from
        while i + p.count <= t.count {
            if t[i] == p[0] && Array(t[i..<i + p.count]) == p { return i }
            i += 1
        }
        return nil
    }

    static func expectFile(_ run: ScenarioRun, _ want: JsonValue) throws {
        let rel = want["path"]?.string ?? ""
        let path = ScenarioRun.path(rel)
        let fs = run.root.fs
        if let exists = want["exists"]?.bool, fs.exists(path) != exists {
            throw ScenarioFailure(exists ? rel : "no " + rel, fs.exists(path) ? "it is there" : "it isn't there")
        }
        if (want.members ?? []).allSatisfy({ $0.key == "path" || $0.key == "exists" }) { return }
        guard fs.exists(path), try !fs.stat(path).isDirectory else { throw ScenarioFailure("the file " + rel, "none") }
        let bytes = try fs.read(path)
        let text = String(decoding: bytes, as: UTF8.self)
        if let wanted = want["text"]?.string {
            let fixture = wanted.hasPrefix("<<shared/fixtures/") && wanted.hasSuffix(">>")
                ? String(wanted.dropFirst("<<shared/fixtures/".count).dropLast(2)) : nil
            let expected = try fixture.map { try Fixtures.text($0) } ?? wanted
            if !text.utf8.elementsEqual(expected.utf8) { throw ScenarioFailure("\(rel) reading \"\(expected)\"", "\"\(text)\"") }
        }
        for c in (want["contains"]?.array ?? []).compactMap(\.string) where !contains(text, c) {
            throw ScenarioFailure("\(rel) containing \"\(c)\"", "\"\(text)\"")
        }
        for c in (want["excludes"]?.array ?? []).compactMap(\.string) where contains(text, c) {
            throw ScenarioFailure("\(rel) without \"\(c)\"", "\"\(text)\"")
        }
        if let json = want["json"] { try same(json, JsonValue.parse(bytes) ?? .null, rel) }
        if let sha = want["sha256"]?.string, Sha256Sums.hash(bytes) != sha { throw ScenarioFailure("\(rel) sha256 \(sha)", Sha256Sums.hash(bytes)) }
        if want["sameAsDisc"] != nil { throw ScenarioFailure("file.sameAsDisc (phase 5)", "not built") }
    }

    /// SHA256SUMS in the folder, every file it lists hashed again; files: exactly the names it lists.
    static func expectSums(_ run: ScenarioRun, _ want: JsonValue) throws {
        let rel = want["folder"]?.string ?? ""
        let folder = ScenarioRun.path(rel)
        let fs = run.root.fs
        let sums = folder + "/" + Sha256Sums.fileName
        guard fs.exists(sums) else { throw ScenarioFailure("\(rel)/\(Sha256Sums.fileName)", "none") }
        let entries = Sha256Sums.parse(String(decoding: try fs.read(sums), as: UTF8.self))
        var bad: [String] = []
        for e in entries {
            let path = folder + "/" + e.path
            if !fs.exists(path) {
                bad.append(e.path + " missing")
            } else {
                let hasher = Sha256Hasher()
                hasher.update(try fs.read(path))
                if hasher.finish() != e.sha256 { bad.append(e.path + " differs") }
            }
        }
        if let ok = want["ok"]?.bool, bad.isEmpty != ok {
            throw ScenarioFailure("sha256sum -c in \(rel) \(ok ? "passing" : "failing")", bad.isEmpty ? "it passes" : bad.joined(separator: ", "))
        }
        if let files = want["files"]?.array {
            try same(.array(sortedBytes(files.compactMap(\.string)).map { .string($0) }), .array(sortedBytes(entries.map(\.path)).map { .string($0) }),
                     "the files \(rel)/SHA256SUMS lists")
        }
    }

    static func expectRecord(_ run: ScenarioRun, _ want: JsonValue, _ named: String?) async throws {
        let name = try want["job"]?.string ?? named ?? onlyJob(run)
        let file = "bromelia-" + String(ArchiveFiles.unitId(try run.job(name)).value.prefix(8)) + ".json"
        let rel: String, path: String
        if let folder = want["folder"] {
            rel = try await fill(run, folder, name).string ?? ""
            path = ScenarioRun.path(rel) + "/" + file
        } else {
            // The unit's folder, wherever it went: the one record of that name under the root.
            let found = try walk(run, TestCompositionRoot.root).files.filter { $0 == file || $0.utf8.reversed().starts(with: ("/" + file).utf8.reversed()) }
            guard found.count == 1 else { throw ScenarioFailure("one record " + file, show(found)) }
            rel = found[0]
            path = TestCompositionRoot.root + "/" + rel
        }
        guard run.root.fs.exists(path) else { throw ScenarioFailure("the record " + path, "none") }
        try same(try await fill(run, want["json"] ?? .object([]), name), JsonValue.parse(try run.root.fs.read(path)) ?? .null, "the record in " + rel)
    }

    /// The job folder's MakeMKV transcript: its runs (==== … $ headers) and the lines it contains.
    static func expectTranscript(_ run: ScenarioRun, _ want: JsonValue, _ named: String?) throws {
        try transcript(run, want, named, SealStep.transcript, "transcript")
    }

    static func expectDebugLog(_ run: ScenarioRun, _ want: JsonValue, _ named: String?) throws {
        try transcript(run, want, named, SealStep.debugTranscript, "debug log")
    }

    private static func transcript(_ run: ScenarioRun, _ want: JsonValue, _ named: String?, _ file: String, _ what: String) throws {
        let name = try want["job"]?.string ?? named ?? onlyJob(run)
        let path = TestCompositionRoot.dataDir + "/jobs/" + (try run.job(name)).value + "/" + file
        guard run.root.fs.exists(path) else { throw ScenarioFailure("the \(what) of \(name)", "none") }
        let text = String(decoding: try run.root.fs.read(path), as: UTF8.self)
        if let runs = want["runs"]?.int {
            let found = ScenarioWorld.lines(text).filter { $0.hasPrefix("==== ") && contains($0, " $ ") }.count
            if found != runs { throw ScenarioFailure("\(runs) runs in the \(what) of \(name)", "\(found):\n" + text) }
        }
        for c in (want["contains"]?.array ?? []).compactMap(\.string) where !contains(text, c) {
            throw ScenarioFailure("the \(what) of \(name) containing \"\(c)\"", text)
        }
        var at = 0
        for c in (want["order"]?.array ?? []).compactMap(\.string) {
            guard let i = find(text, c, from: at) else { throw ScenarioFailure("the \(what) of \(name) containing \"\(c)\" after what came before", text) }
            at = i + c.utf8.count
        }
    }

    // MARK: Calls, log, events, problems

    static func expectCalls(_ run: ScenarioRun, _ want: JsonValue) throws {
        for (tool, w) in want.members ?? [] {
            let calls = run.tools.calls(tool)
            func found() -> String { "\(calls.count) calls: " + calls.joined(separator: "\n  ") }
            if w["none"]?.bool == true && !calls.isEmpty { throw ScenarioFailure("no \(tool) call", found()) }
            if let count = w["count"]?.int, calls.count != count { throw ScenarioFailure("\(count) \(tool) calls", found()) }
            for s in (w["include"]?.array ?? []).compactMap(\.string) where !calls.contains(where: { contains($0, s) }) {
                throw ScenarioFailure("a \(tool) call with \"\(s)\"", found())
            }
            for s in (w["exclude"]?.array ?? []).compactMap(\.string) where calls.contains(where: { contains($0, s) }) {
                throw ScenarioFailure("no \(tool) call with \"\(s)\"", found())
            }
            for (s, n) in w["counts"]?.members ?? [] where Int64(calls.filter({ contains($0, s) }).count) != n.int {
                throw ScenarioFailure("\(n.int ?? 0) \(tool) calls with \"\(s)\"", found())
            }
        }
    }

    static func expectLog(_ run: ScenarioRun, _ want: JsonValue, _ named: String?) async throws {
        let name = try want["job"]?.string ?? named ?? onlyJob(run)
        let codes = try await run.root.engine.jobLog(try run.job(name), after: 0).map(\.code.rawValue)
        for c in (want["include"]?.array ?? []).compactMap(\.string) where !codes.contains(c) {
            throw ScenarioFailure("\(c) in the log of \(name)", show(codes))
        }
        for c in (want["exclude"]?.array ?? []).compactMap(\.string) where codes.contains(c) {
            throw ScenarioFailure("no \(c) in the log of \(name)", show(codes))
        }
    }

    /// An event as an expectation reads it: its data's members and its type.
    private static func eventJson(_ e: EngineEvent) -> JsonValue { .object((e.data.members ?? []) + [("type", .string(e.type.rawValue))]) }

    static func expectEvents(_ run: ScenarioRun, _ want: JsonValue) throws {
        let events = run.allEvents()
        let types = events.map(\.type.rawValue)
        for w in want["include"]?.array ?? [] where !events.contains(where: { matches(w, eventJson($0)) }) {
            throw ScenarioFailure("an event " + ScenarioRun.jsonText(w), show(types))
        }
        var at = 0
        for t in (want["sequence"]?.array ?? []).compactMap(\.string) {
            guard let i = types[at...].firstIndex(of: t) else {
                throw ScenarioFailure("\(t) in the sequence \(ScenarioRun.jsonText(want["sequence"]))", show(types))
            }
            at = i + 1
        }
        for t in (want["exclude"]?.array ?? []).compactMap(\.string) where types.contains(t) { throw ScenarioFailure("no " + t, show(types)) }
    }

    static func expectProblems(_ run: ScenarioRun, _ want: JsonValue) async throws {
        let active = try await run.root.engine.system().problems.map(\.message.code.rawValue)
        if want["none"]?.bool == true && !active.isEmpty { throw ScenarioFailure("no problems", show(active)) }
        for c in (want["active"]?.array ?? []).compactMap(\.string) where !active.contains(c) { throw ScenarioFailure("the problem " + c, show(active)) }
        for c in (want["cleared"]?.array ?? []).compactMap(\.string) {
            if active.contains(c) { throw ScenarioFailure("\(c) cleared", "still active") }
            if !run.allEvents().contains(where: { $0.type == .problemCleared && contains($0.data.description, "\"" + c + "\"") }) {
                throw ScenarioFailure("a problemCleared for \(c)", "none")
            }
        }
    }

    /// Each drive by its scenario id: a fake drive's through its device, else a configured one's through its configId.
    static func expectDrives(_ run: ScenarioRun, _ want: JsonValue) async throws {
        let drives = try await run.root.engine.drives()
        for w in want.array ?? [] {
            let id = w["id"]?.string ?? ""
            let device = run.world.drives.first { $0.name == id }?.device
            guard let drive = drives.first(where: { device != nil ? $0.device == device : $0.configId == id }) else {
                throw ScenarioFailure("the drive " + id, ScenarioRun.jsonText(.array(drives.map(DriveDto.json))))
            }
            try same(.object((w.members ?? []).filter { $0.key != "id" }), DriveDto.json(drive), "the drive " + id)
        }
    }

    static func expectConfig(_ run: ScenarioRun, _ want: JsonValue) async throws {
        let envelope = try await run.root.engine.config()
        try same(want, envelope.config.document, "the configuration")
    }

    // MARK: Db

    /// A table's rows, in rowid order, through a read-only connection of its own.
    private static func db(_ run: ScenarioRun, _ table: String) throws -> [JsonValue] {
        guard !table.isEmpty, table.utf8.allSatisfy({ ($0 >= 97 && $0 <= 122) || $0 == 95 }) else { throw ScenarioFailure("a table name", table) }
        var handle: OpaquePointer?
        guard sqlite3_open_v2(run.root.folder + "/bromelia.db", &handle, SQLITE_OPEN_READONLY, nil) == SQLITE_OK, let db = handle else {
            sqlite3_close(handle)
            throw ScenarioFailure("the database open", "it couldn't be opened")
        }
        defer { sqlite3_close(db) }
        var statement: OpaquePointer?
        guard sqlite3_prepare_v2(db, "SELECT * FROM \(table) ORDER BY rowid", -1, &statement, nil) == SQLITE_OK, let s = statement else {
            throw ScenarioFailure("the table \(table)", String(cString: sqlite3_errmsg(db)))
        }
        defer { sqlite3_finalize(s) }
        var rows: [JsonValue] = []
        while sqlite3_step(s) == SQLITE_ROW {
            var m: [(key: String, value: JsonValue)] = []
            for i in 0..<sqlite3_column_count(s) {
                let name = String(cString: sqlite3_column_name(s, i))
                let value: JsonValue
                switch sqlite3_column_type(s, i) {
                case SQLITE_NULL: value = .null
                case SQLITE_INTEGER: value = .integer(sqlite3_column_int64(s, i))
                case SQLITE_FLOAT: value = .number(sqlite3_column_double(s, i))
                case SQLITE_BLOB:
                    let count = Int(sqlite3_column_bytes(s, i))
                    let bytes = sqlite3_column_blob(s, i).map { Array(UnsafeRawBufferPointer(start: $0, count: count)) } ?? []
                    value = .string(bytes.map { String(format: "%02x", $0) }.joined())
                default:
                    value = .string(sqlite3_column_text(s, i).map { String(cString: $0) } ?? "")
                }
                m.append((name, value))
            }
            rows.append(.object(m))
        }
        return rows
    }

    static func expectDb(_ run: ScenarioRun, _ want: JsonValue) throws {
        for (table, w) in want.members ?? [] {
            let rows = try db(run, table)
            if let count = w["count"]?.int, rows.count != count {
                throw ScenarioFailure("\(count) rows in \(table)", "\(rows.count): " + ScenarioRun.jsonText(.array(rows)))
            }
            if let wanted = w["rows"] { try same(wanted, .array(rows), "the rows of " + table) }
        }
    }

    // MARK: Matching and placeholders

    /// {root}, {unit8} / {jobId} (of the job named in the step, else the only job), {unit8:<name>}, {jobId:<name>},
    /// {driveId:<drive>} in every string.
    static func fill(_ run: ScenarioRun, _ v: JsonValue, _ named: String?) async throws -> JsonValue {
        switch v {
        case .string(let s): return .string(try await fill(run, s, named))
        case .array(let a):
            var out: [JsonValue] = []
            for x in a { out.append(try await fill(run, x, named)) }
            return .array(out)
        case .object(let m):
            var out: [(key: String, value: JsonValue)] = []
            for x in m { out.append((x.key, try await fill(run, x.value, named))) }
            return .object(out)
        default: return v
        }
    }

    static func fill(_ run: ScenarioRun, _ text: String, _ named: String?) async throws -> String {
        let s = text.replacingOccurrences(of: "{root}", with: TestCompositionRoot.root)
        guard s.contains("{") else { return s }
        var out = ""
        var rest = Substring(s)
        while let open = rest.firstIndex(of: "{") {
            out += rest[..<open]
            guard let close = rest[open...].firstIndex(of: "}") else {
                rest = rest[open...]
                break
            }
            let inner = String(rest[rest.index(after: open)..<close])
            let colon = inner.firstIndex(of: ":")
            let head = colon.map { String(inner[..<$0]) } ?? inner
            let arg = colon.map { String(inner[inner.index(after: $0)...]) }
            if (head == "unit8" || head == "jobId") && arg?.isEmpty != true {
                let id = try run.job(try arg ?? named ?? onlyJob(run))
                out += head == "jobId" ? id.value : String(ArchiveFiles.unitId(id).value.prefix(8))
            } else if head == "driveId", let arg, !arg.isEmpty {
                out += try await run.driveId(arg)
            } else {
                out += rest[open...close]
            }
            rest = rest[rest.index(after: close)...]
        }
        return out + rest
    }

    private static func same(_ want: JsonValue, _ got: JsonValue?, _ what: String) throws {
        if !matches(want, got) { throw ScenarioFailure("\(what) \(ScenarioRun.jsonText(want))", ScenarioRun.jsonText(got)) }
    }

    /// Objects: every wanted member matches (a wanted null: absent or null). Arrays: the same length, element by element.
    /// Numbers by value. A wanted string with '*' in it: a glob ('*' any run of characters; owner H3b-5). Anything else:
    /// equal (strings byte for byte).
    static func matches(_ want: JsonValue, _ got: JsonValue?) -> Bool {
        switch want {
        case .null:
            return got == nil || got!.isNull
        case .string(let w):
            guard case .string(let g)? = got else { return false }
            return w.utf8.contains(UInt8(ascii: "*")) ? starGlob(Array(w.utf8), Array(g.utf8)) : w.utf8.elementsEqual(g.utf8)
        case .object(let m):
            guard let got, got.members != nil else { return false }
            return m.allSatisfy { matches($0.value, got[$0.key]) }
        case .array(let a):
            guard let ga = got?.array, ga.count == a.count else { return false }
            return zip(a, ga).allSatisfy { matches($0, $1) }
        case .integer, .number:
            guard let n = got?.double else { return false }
            return want.double == n
        default:
            return got == want
        }
    }

    /// '*' any run of bytes (newlines and '/' too), everything else itself.
    private static func starGlob(_ p: [UInt8], _ t: [UInt8]) -> Bool {
        func match(_ i: Int, _ j: Int) -> Bool {
            if i == p.count { return j == t.count }
            if p[i] == UInt8(ascii: "*") { return (j...t.count).contains { match(i + 1, $0) } }
            return j < t.count && p[i] == t[j] && match(i + 1, j + 1)
        }
        return match(0, 0)
    }
}
