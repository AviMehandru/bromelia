import BroAdapters
import BroDomain
import BroEngine
import BroFoundation
import Bromeliad
import Foundation
import Testing

/// The realDisc shared scenarios (decisions 302–304) through the real root: real makemkvcon and mkvmerge, real disk, the image
/// only read. Every scenario whose phase is built is a test case: when its variable isn't set it says it is skipped with the
/// reason (Swift Testing has no skip at run time), and a failure is a failure when it is. Nothing of the user's is read: a
/// scratch world, an empty home, PATH alone from the environment, no system keystore, no drives. Exactly what the phase 3
/// real-disc scenarios use is supported; anything else fails as "not supported in a real world: <key>", so a later one can't
/// pass half-read.
@Suite(.serialized)
struct RealDiscScenarioTests {
    @Test(arguments: RealDiscCatalog.selected())
    func playsARealDiscScenario(_ id: String) async throws {
        guard RealDiscCatalog.image(id) != nil else {
            print("skipped: " + RealDiscCatalog.skipReason(id))
            return
        }
        do {
            try await RealDiscRun(id, try RealDiscCatalog.load(id)).play()
        } catch {
            Issue.record("\(error)")
        }
    }

    /// A later realDisc scenario can't pass half-read: the phase 5 ones fail before anything runs.
    @Test(arguments: [
        ("realdisc-custom-tracks-rename-script", "realdisc-custom-tracks-rename-script step 0 (start): not supported in a real world: given.configPatch"),
        ("realdisc-dvd-navigation-from-iso", "realdisc-dvd-navigation-from-iso step 1 (open): not supported in a real world: open"),
    ])
    func whatAPhase3RunDoesntKnowFails(_ id: String, _ failure: String) throws {
        let found = #expect(throws: RealDiscFailure.self) { try RealDiscRun(id, try RealDiscCatalog.load(id)).checkSupported() }
        #expect(found?.message == failure)
    }
}

/// shared/scenarios/json: the scenarios with realDisc whose phase is built.
enum RealDiscCatalog {
    private static let builtPhase: Int64 = 3

    static let shared: String = {
        var dir = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        while dir.path != "/" {
            let candidate = dir.appendingPathComponent("shared").path
            if FileManager.default.fileExists(atPath: candidate + "/scenarios/json") { return candidate }
            dir = dir.deletingLastPathComponent()
        }
        fatalError("shared/scenarios/json not found above " + #filePath)
    }()

    static func read(_ path: String) throws -> JsonValue {
        let bytes = try Data(contentsOf: URL(fileURLWithPath: shared + "/" + path))
        guard let value = JsonValue.parse([UInt8](bytes)) else { throw RealDiscFailure(path + " isn't JSON") }
        return value
    }

    static func load(_ id: String) throws -> JsonValue { try read("scenarios/json/" + id + ".json") }

    static func selected() -> [String] {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: shared + "/scenarios/json")) ?? []
        return names.filter { $0.hasSuffix(".json") }.map { String($0.dropLast(5)) }.filter { id in
            guard let s = try? load(id), s["realDisc"] != nil, let phase = s["phase"]?.int else { return false }
            return phase <= builtPhase
        }.sorted()
    }

    private static func variable(_ id: String) -> String { (try? load(id))?["realDisc"]?["env"]?.string ?? "" }

    /// The image the scenario's variable names; nil when it isn't set.
    static func image(_ id: String) -> String? {
        guard let path = ProcessInfo.processInfo.environment[variable(id)], !path.isEmpty else { return nil }
        return path
    }

    /// Why it doesn't run here, with the room it would have (realdisc-one-pass writes 13.8 GB).
    static func skipReason(_ id: String) -> String {
        let temp = NSTemporaryDirectory()
        let attributes = try? FileManager.default.attributesOfFileSystem(forPath: temp)
        let free = ((attributes?[.systemFreeSize] as? NSNumber)?.doubleValue ?? 0) / 1e9
        return "\(id): \(variable(id)) isn't set (\(String(format: "%.1f", free)) GB free in \(temp))"
    }
}

/// A step of a real run didn't hold: what was expected, and what was found.
struct RealDiscFailure: Error, CustomStringConvertible {
    let message: String

    init(_ expected: String, _ found: String) { message = expected + " / " + found }

    init(_ message: String) { self.message = message }

    var description: String { message }
}

/// One scenario through the real root (decision 302): a fresh world under the temporary folder (one volume, so staging →
/// library is a rename), removed afterwards; on a failure the job's logs and transcripts are copied out first.
final class RealDiscRun {
    private let id: String
    private let scenario: JsonValue
    private let root = URL(fileURLWithPath: NSTemporaryDirectory()).resolvingSymlinksInPath().path + "/bromelia-realdisc-" + UUID().uuidString
    private var jobs: [(name: String, id: Id)] = []
    private let started = Date()
    private var engine: LocalEngine!
    private var step = 0
    private var kind = "start"
    private var data: String { root + "/data" }
    private var home: String { root + "/home" }
    private var library: String { root + "/library" }

    init(_ id: String, _ scenario: JsonValue) {
        self.id = id
        self.scenario = scenario
    }

    func play() async throws {
        try checkSupported()
        do {
            try prepare()
            engine = try await CompositionRoot.build(RootOptions(dataDir: data, configPath: nil, home: home, systemKeystore: false, systemTools: true,
                                                                 drives: false, environment: ["PATH": ProcessInfo.processInfo.environment["PATH"] ?? ""]))
            try await engine.start()
            for s in scenario["steps"]?.array ?? [] {
                step += 1
                guard let (k, value) = s.members?.first else { continue }
                kind = k
                if k == "rip" { try await rip(value) } else { try await expect(value) }
            }
            try await finish()
        } catch {
            keepLogs()
            await stop()
            try? FileManager.default.removeItem(atPath: root)
            if let f = error as? RealDiscFailure { throw RealDiscFailure("\(id) step \(step) (\(kind)): \(f.message)") }
            if let b = error as? BroError { throw RealDiscFailure("\(id) step \(step) (\(kind)): error \(b.code) \(JsonValue.object(b.params))") }
            throw RealDiscFailure("\(id) step \(step) (\(kind)): \(error)")
        }
        await stop()
        try? FileManager.default.removeItem(atPath: root)
    }

    // MARK: What a real world supports

    private func only(_ place: String, _ value: JsonValue, _ keys: String...) throws {
        for (key, _) in value.members ?? [] where !keys.contains(key) {
            throw RealDiscFailure("not supported in a real world: " + place + key)
        }
    }

    func checkSupported() throws {
        do {
            try only("", scenario, "title", "phase", "realDisc", "from", "given", "steps")
            try only("given.", scenario["given"] ?? .object([]), "profile")
            for s in scenario["steps"]?.array ?? [] {
                step += 1
                guard let (k, value) = s.members?.first else { continue }
                kind = k
                if k == "rip" {
                    try only("rip.", value, "iso", "as")
                } else if k == "expect" {
                    try only("expect.", value, "job", "jobs", "log", "calls", "files", "sums")
                    if let job = value["job"] { try only("expect.job.", job, "name", "state", "outcome", "error", "noError", "files") }
                    if let all = value["jobs"] { try only("expect.jobs.", all, "count", "outcomes", "states", "kinds") }
                    if let log = value["log"] { try only("expect.log.", log, "job", "include", "exclude") }
                    if let calls = value["calls"] {
                        try only("expect.calls.", calls, "makemkvcon")
                        try only("expect.calls.makemkvcon.", calls["makemkvcon"] ?? .null, "none", "count", "include", "exclude", "counts")
                    }
                    if let files = value["files"] { try only("expect.files.", files, "under", "exactly", "include", "exclude") }
                    if let sums = value["sums"] { try only("expect.sums.", sums, "folder", "ok", "files") }
                } else {
                    throw RealDiscFailure("not supported in a real world: " + k)
                }
            }
            step = 0
            kind = "start"
        } catch let f as RealDiscFailure {
            throw RealDiscFailure("\(id) step \(step) (\(kind)): \(f.message)")
        }
    }

    // MARK: The world

    private func prepare() throws {
        let fm = FileManager.default
        for folder in [data, home, library] { try fm.createDirectory(atPath: folder, withIntermediateDirectories: true) }
        let marker = LibraryMarker.make("lib", path: library, name: "Library", created: SystemClock().now())
        try Data(LibraryMarker.encode(marker)).write(to: URL(fileURLWithPath: library + "/" + LibraryMarker.fileName))
        try Data(JsonValue.encodeCanonical(try configuration())).write(to: URL(fileURLWithPath: data + "/config.json"))
    }

    /// config3-scenario-base.json with the scenario's profile over the default one, {root} filled in.
    private func configuration() throws -> JsonValue {
        var config = try RealDiscCatalog.read("fixtures/config/config3-scenario-base.json")
        if let profile = scenario["given"]?["profile"] {
            var profiles = config["profiles"]?.array ?? []
            if let at = profiles.firstIndex(where: { $0["id"]?.string == "default" }) { profiles[at] = Self.mergePatch(profiles[at], profile) }
            config = Self.mergePatch(config, .object([("profiles", .array(profiles))]))
        }
        return fill(config)
    }

    /// RFC 7386.
    private static func mergePatch(_ target: JsonValue, _ patch: JsonValue) -> JsonValue {
        guard let p = patch.members else { return patch }
        var members = target.members ?? []
        for (key, value) in p {
            let at = members.firstIndex { $0.key == key }
            if value.isNull {
                if let at { members.remove(at: at) }
                continue
            }
            let merged = mergePatch(at.map { members[$0].value } ?? .null, value)
            if let at { members[at] = (key, merged) } else { members.append((key, merged)) }
        }
        return .object(members)
    }

    private func fill(_ v: JsonValue) -> JsonValue {
        switch v {
        case .string(let s): return .string(s.replacingOccurrences(of: "{root}", with: root))
        case .array(let a): return .array(a.map(fill))
        case .object(let m): return .object(m.map { ($0.key, fill($0.value)) })
        default: return v
        }
    }

    // MARK: The steps

    private func rip(_ v: JsonValue) async throws {
        let iso = RealDiscCatalog.image(id) ?? ""
        let made = try await engine.startJob(JobRequest(kind: .videoDisc, title: URL(fileURLWithPath: iso).deletingPathExtension().lastPathComponent,
                                                        automatic: false, options: .object([]),
                                                        details: DiscRequest.encode(DiscRequest(source: .iso(path: iso), session: nil))))
        jobs.append((v["as"]?.string ?? "job\(jobs.count + 1)", made.id))
    }

    /// Real time (decision 302): until every job has finished, or the cap, which fails with the state and the step.
    private func settle() async throws {
        let minutes = id == "realdisc-one-pass" ? 60.0 : 10.0
        let deadline = Date().addingTimeInterval(minutes * 60)
        for (name, job) in jobs {
            while true {
                let detail = try await engine.job(job)
                if detail.job.state == .finished { break }
                if Date() > deadline {
                    throw RealDiscFailure("job \(name) to finish within \(Int(minutes)) min", detail.job.state.rawValue + ", steps "
                                          + detail.steps.map { $0.kind.rawValue + ":" + $0.state.rawValue }.joined(separator: " "))
                }
                try await Task.sleep(nanoseconds: 1_000_000_000)
            }
        }
    }

    private func expect(_ expect: JsonValue) async throws {
        try await settle()
        let named = expect["job"]?["name"]?.string
        for (key, want) in expect.members ?? [] {
            switch key {
            case "job": try await expectJob(want)
            case "jobs": try await expectJobs(want)
            case "log": try await expectLog(want, named)
            case "calls": try expectCalls(want["makemkvcon"] ?? .null, named)
            case "files": try expectFiles(want)
            case "sums": try expectSums(want)
            default: break
            }
        }
    }

    private func onlyJob(_ named: String?) throws -> String {
        if let named { return named }
        if jobs.count == 1 { return jobs[0].name }
        throw RealDiscFailure("one job, or a name", "\(jobs.count) jobs")
    }

    private func job(_ name: String) throws -> Id {
        guard let found = jobs.first(where: { $0.name == name }) else { throw RealDiscFailure("a job named " + name, "none") }
        return found.id
    }

    private func expectJob(_ want: JsonValue) async throws {
        let name = try onlyJob(want["name"]?.string)
        let detail = try await engine.job(try job(name))
        let json = JobDetailDto.json(detail)
        do {
            for (key, value) in want.members ?? [] {
                switch key {
                case "state", "outcome", "error":
                    try same(value, json[key], "job \(name) \(key)")
                case "noError":
                    if value.bool == true, let error = json["error"], !error.isNull { throw RealDiscFailure("job \(name) without an error", text(error)) }
                case "files":
                    try same(value, .integer(Int64(detail.files.count)), "job \(name) files")
                default:
                    break
                }
            }
        } catch let f as RealDiscFailure {
            let log = try await engine.jobLog(try job(name), after: 0).map { l in l.code.rawValue + (l.params.isEmpty ? "" : " " + text(.object(l.params))) }
            throw RealDiscFailure(f.message + "\n  (job \(name): \(detail.job.state.rawValue), error \(text(json["error"])), steps "
                                  + detail.steps.map { $0.kind.rawValue + ":" + $0.state.rawValue }.joined(separator: " ") + ", log:\n    "
                                  + log.joined(separator: "\n    ") + ")")
        }
    }

    private func expectJobs(_ want: JsonValue) async throws {
        let all = try await engine.jobs(JobQueryDto(limit: JobQueryDto.maxLimit)).items
        if let count = want["count"]?.int { try same(.integer(count), .integer(Int64(all.count)), "jobs count") }
        if let outcomes = want["outcomes"] { try same(outcomes, .array(all.map { $0.outcome.map { .string($0.rawValue) } ?? .null }), "jobs outcomes") }
        if let states = want["states"] { try same(states, .array(all.map { .string($0.state.rawValue) }), "jobs states") }
        if let kinds = want["kinds"] { try same(kinds, .array(all.map { .string($0.kind.rawValue) }), "jobs kinds") }
    }

    private func expectLog(_ want: JsonValue, _ named: String?) async throws {
        let name = try onlyJob(want["job"]?.string ?? named)
        let codes = try await engine.jobLog(try job(name), after: 0).map(\.code.rawValue)
        for c in (want["include"]?.array ?? []).compactMap(\.string) where !codes.contains(c) {
            throw RealDiscFailure("\(c) in the log of \(name)", show(codes))
        }
        for c in (want["exclude"]?.array ?? []).compactMap(\.string) where codes.contains(c) {
            throw RealDiscFailure("no \(c) in the log of \(name)", show(codes))
        }
    }

    /// The makemkvcon runs of the job (decision 304): each run's "==== … $ <command>" header in its makemkv.txt.
    private func calls(_ job: Id) -> [String] {
        guard let text = try? String(contentsOfFile: data + "/jobs/" + job.value + "/" + SealStep.transcript, encoding: .utf8) else { return [] }
        return text.unicodeScalars.split(separator: "\n").map { String($0) }.compactMap { line in
            guard line.hasPrefix("==== "), let at = line.range(of: " $ ") else { return nil }
            return String(line[at.upperBound...])
        }
    }

    private func expectCalls(_ w: JsonValue, _ named: String?) throws {
        let found = calls(try job(try onlyJob(named)))
        func shown() -> String { "\(found.count) calls: " + found.joined(separator: "\n  ") }
        if w["none"]?.bool == true && !found.isEmpty { throw RealDiscFailure("no makemkvcon call", shown()) }
        if let count = w["count"]?.int, found.count != Int(count) { throw RealDiscFailure("\(count) makemkvcon calls", shown()) }
        for s in (w["include"]?.array ?? []).compactMap(\.string) where !found.contains(where: { $0.contains(s) }) {
            throw RealDiscFailure("a makemkvcon call with \"\(s)\"", shown())
        }
        for s in (w["exclude"]?.array ?? []).compactMap(\.string) where found.contains(where: { $0.contains(s) }) {
            throw RealDiscFailure("no makemkvcon call with \"\(s)\"", shown())
        }
        for (s, n) in w["counts"]?.members ?? [] where Int64(found.filter({ $0.contains(s) }).count) != n.int {
            throw RealDiscFailure("\(n) makemkvcon calls with \"\(s)\"", shown())
        }
    }

    /// Every file and folder under the folder, relative to it, folders without a final '/'.
    private func walk(_ folder: String) -> (files: [String], folders: [String]) {
        var files: [String] = [], folders: [String] = []
        func visit(_ dir: String, _ rel: String) {
            for name in (try? FileManager.default.contentsOfDirectory(atPath: dir)) ?? [] {
                let r = rel.isEmpty ? name : rel + "/" + name
                var isDirectory: ObjCBool = false
                _ = FileManager.default.fileExists(atPath: dir + "/" + name, isDirectory: &isDirectory)
                if isDirectory.boolValue {
                    folders.append(r)
                    visit(dir + "/" + name, r)
                } else {
                    files.append(r)
                }
            }
        }
        visit(folder, "")
        return (files, folders)
    }

    private func hidden(_ path: String) -> Bool { path.split(separator: "/").contains { $0.hasPrefix(".") } }

    private func trimSlash(_ s: String) -> String { s.hasSuffix("/") ? String(s.dropLast()) : s }

    private func expectFiles(_ want: JsonValue) throws {
        let (files, folders) = walk(want["under"]?.string.map { root + "/" + trimSlash($0) } ?? root)
        let all = files + folders
        let place = want["under"]?.string.map { " under " + $0 } ?? ""
        if let exactly = want["exactly"]?.array {
            let empty = folders.filter { d in !hidden(d) && !all.contains { $0.hasPrefix(d + "/") } }.map { $0 + "/" }
            let listed = (files.filter { !hidden($0) } + empty).sorted()
            let patterns = exactly.compactMap(\.string)
            if !(listed.allSatisfy { f in patterns.contains { glob($0, f) } } && patterns.allSatisfy { p in listed.contains { glob(p, $0) } }) {
                throw RealDiscFailure("exactly \(show(patterns.sorted()))\(place)", show(listed))
            }
        }
        for p in (want["include"]?.array ?? []).compactMap(\.string) where !all.contains(where: { glob(p, $0) }) {
            throw RealDiscFailure("\(p)\(place)", show(all.filter { !hidden($0) || p.contains(".") }.sorted()))
        }
        for p in (want["exclude"]?.array ?? []).compactMap(\.string) {
            let found = all.filter { glob(p, $0) }
            if !found.isEmpty { throw RealDiscFailure("no \(p)\(place)", show(found)) }
        }
    }

    /// A path pattern: * within one component, ** across components; a component starting with '.' is matched only by a
    /// pattern component that writes the dot.
    private func glob(_ pattern: String, _ path: String) -> Bool {
        let pats = trimSlash(pattern).split(separator: "/", omittingEmptySubsequences: false).map(String.init)
        let parts = trimSlash(path).split(separator: "/", omittingEmptySubsequences: false).map(String.init)
        return globParts(pats, 0, parts, 0) && (!pattern.hasSuffix("/") || path.hasSuffix("/"))
    }

    private func globParts(_ pats: [String], _ i: Int, _ parts: [String], _ j: Int) -> Bool {
        if i == pats.count { return j == parts.count }
        if pats[i] == "**" { return (j...parts.count).contains { globParts(pats, i + 1, parts, $0) } }
        if j == parts.count { return false }
        if parts[j].hasPrefix(".") && !pats[i].hasPrefix(".") { return false }
        return fnmatch(pats[i], parts[j], 0) == 0 && globParts(pats, i + 1, parts, j + 1)
    }

    /// What's wrong with the folder's SHA256SUMS, every file it lists hashed again from disk, and what it lists.
    private func checkSums(_ folder: String) throws -> (bad: [String], entries: [SumEntry]) {
        let entries = Sha256Sums.parse(try String(contentsOfFile: folder + "/" + Sha256Sums.fileName, encoding: .utf8))
        var bad: [String] = []
        for e in entries {
            guard let file = FileHandle(forReadingAtPath: folder + "/" + e.path) else {
                bad.append(e.path + " missing")
                continue
            }
            defer { try? file.close() }
            let hasher = Sha256Hasher()
            while true {
                let more: Bool = try autoreleasepool {
                    guard let chunk = try file.read(upToCount: 1 << 22), !chunk.isEmpty else { return false }
                    hasher.update([UInt8](chunk))
                    return true
                }
                if !more { break }
            }
            if hasher.finish() != e.sha256 { bad.append(e.path + " differs") }
        }
        return (bad, entries)
    }

    private func expectSums(_ want: JsonValue) throws {
        let rel = want["folder"]?.string ?? ""
        let folder = root + "/" + rel
        guard FileManager.default.fileExists(atPath: folder + "/" + Sha256Sums.fileName) else { throw RealDiscFailure("\(rel)/\(Sha256Sums.fileName)", "none") }
        let (bad, entries) = try checkSums(folder)
        if let ok = want["ok"]?.bool, bad.isEmpty != ok {
            throw RealDiscFailure("sha256sum -c in \(rel) \(ok ? "passing" : "failing")", bad.isEmpty ? "it passes" : bad.joined(separator: ", "))
        }
        if let files = want["files"]?.array {
            try same(.array(files.compactMap(\.string).sorted().map { .string($0) }), .array(entries.map(\.path).sorted().map { .string($0) }),
                     "the files \(rel)/SHA256SUMS lists")
        }
    }

    private func show(_ items: [String]) -> String { "[" + items.map { "\"\($0)\"" }.joined(separator: ", ") + "]" }

    private func text(_ v: JsonValue?) -> String { v.map(\.description) ?? "null" }

    private func same(_ want: JsonValue, _ got: JsonValue?, _ what: String) throws {
        if !matches(want, got) { throw RealDiscFailure("\(what) \(text(want))", text(got)) }
    }

    /// Objects by every wanted member, arrays element by element, numbers by value.
    private func matches(_ want: JsonValue, _ got: JsonValue?) -> Bool {
        switch want {
        case .null:
            return got == nil || got!.isNull
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

    // MARK: The end

    /// What every real run must leave true, whatever its scenario says: no makemkvcon run named a drive, nothing was written to
    /// the empty home, every SHA256SUMS in the library holds from disk. Then the run's line for the report.
    private func finish() async throws {
        step += 1
        kind = "end"
        for (name, job) in jobs {
            for call in calls(job) where call.contains(" disc:") || call.contains(" dev:") {
                throw RealDiscFailure("job \(name) reading only the image", call)
            }
        }
        let left = (try? FileManager.default.contentsOfDirectory(atPath: home)) ?? []
        if !left.isEmpty { throw RealDiscFailure("the home left empty", show(left)) }
        if !(try await engine.drives().isEmpty) { throw RealDiscFailure("no drives", "some") }
        let files = walk(library).files
        let sums = files.filter { $0.hasSuffix("/" + Sha256Sums.fileName) }
        for s in sums {
            let (bad, _) = try checkSums(library + "/" + (s as NSString).deletingLastPathComponent)
            if !bad.isEmpty { throw RealDiscFailure("sha256sum -c passing in " + s, bad.joined(separator: ", ")) }
        }
        let bytes = files.reduce(Int64(0)) { sum, f in
            sum + (((try? FileManager.default.attributesOfItem(atPath: library + "/" + f))?[.size] as? NSNumber)?.int64Value ?? 0)
        }
        let seconds = Int(Date().timeIntervalSince(started))
        let shown = files.filter { !hidden($0) }
        print("\(id): \(seconds) s, \(shown.count) files in the library (\(bytes) bytes), \(sums.count) SHA256SUMS hold: \(show(shown.sorted()))")
    }

    private func stop() async {
        guard let engine else { return }
        do {
            try await engine.stop()
        } catch {
            print("\(id): stopping the engine: \(error)")
        }
    }

    /// The engine log and each job folder's own files (its logs and transcripts; never its home), to BROMELIA_TEST_OUTPUT or
    /// the temporary folder.
    private func keepLogs() {
        let fm = FileManager.default
        let base = ProcessInfo.processInfo.environment["BROMELIA_TEST_OUTPUT"].flatMap { $0.isEmpty ? nil : $0 } ?? NSTemporaryDirectory()
        let output = base + "/bromelia-realdisc-failed/" + id
        try? fm.createDirectory(atPath: output, withIntermediateDirectories: true)
        try? fm.removeItem(atPath: output + "/engine.log")
        try? fm.copyItem(atPath: data + "/logs/engine.log", toPath: output + "/engine.log")
        for (_, job) in jobs {
            let folder = data + "/jobs/" + job.value
            for name in (try? fm.contentsOfDirectory(atPath: folder)) ?? [] {
                var isDirectory: ObjCBool = false
                guard fm.fileExists(atPath: folder + "/" + name, isDirectory: &isDirectory), !isDirectory.boolValue else { continue }
                let to = output + "/" + job.value + "-" + name
                try? fm.removeItem(atPath: to)
                try? fm.copyItem(atPath: folder + "/" + name, toPath: to)
            }
        }
        print("\(id): logs kept in \(output)")
    }
}
