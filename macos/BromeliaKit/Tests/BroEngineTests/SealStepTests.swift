import BroDomain
import BroEngine
import BroFoundation
import BroPorts
import BroTestSupport
import Foundation
import Testing

private let jobIdText = "00000000-0000-4000-8000-000000000001"

/// The progress, the log, the library and the clock a Seal case gives the step.
private final class SealFakes: ProgressReporter, JobLogger, LibraryPort, Clock, DecisionPort, Checkpointer, @unchecked Sendable {
    let given: JsonValue
    let fs: MemoryFileSystem
    private let lock = NSLock()
    private var _log: [BroMessage] = []
    private var _progress: [JsonValue] = []

    init(_ given: JsonValue, _ fs: MemoryFileSystem) {
        self.given = given
        self.fs = fs
    }

    var logged: [BroMessage] { lock.withLock { _log } }
    var progressed: [JsonValue] { lock.withLock { _progress } }

    func clear() {
        lock.withLock {
            _log.removeAll()
            _progress.removeAll()
        }
    }

    func report(_ fraction: Double, operation: BroMessage, current: String?) {
        var item: [JsonValue] = [.number(fraction), .string(operation.code.rawValue)]
        if let current { item.append(.string(current)) }
        lock.withLock { _progress.append(.array(item)) }
    }

    func log(_ message: BroMessage) { lock.withLock { _log.append(message) } }

    func require(_ libraryId: String) throws(BroError) -> LibraryHandle {
        guard let l = given["libraries"]?[libraryId] else { throw BroError(MessageCode.wire(.libraryNotFound)) }
        if let why = l["offline"] { throw BroError(why["code"]?.string ?? "", why["params"]?.members ?? []) }
        return LibraryHandle(id: libraryId, path: l["path"]?.string ?? "")
    }

    func stagingDirectory(_ libraryId: String, jobId: Id) throws(BroError) -> String {
        let path = try require(libraryId).path + "/.bromelia/staging/" + jobId.value
        fs.putFolder(path)
        return path
    }

    func commit(_ unit: UnitRecord, intent: CommitIntent, files: [UnitFile]) async throws(BroError) -> CommitResult { fatalError("not used") }
    func quarantine(_ unit: UnitRecord, intent: CommitIntent, files: [UnitFile], tag: String) async throws(BroError) -> CommitResult {
        fatalError("not used")
    }

    func now() -> Instant { Instant.parse(given["now"]?.string ?? "")! }
    func monotonic() -> Duration { fatalError("not used") }
    func sleep(_ duration: Duration, cancel: CancellationToken) async throws(BroError) { fatalError("not used") }
    func timer(_ schedule: TimerSchedule, handler: @escaping @Sendable () -> Void) -> any TimerHandle { fatalError("not used") }
    /// Seal's times are UTC (decision 59).
    func localTime(_ instant: Instant) -> LocalTime { fatalError("not used") }

    func ask(_ decision: DecisionDto) async throws(BroError) -> String { fatalError("not used") }
    var saved: JsonValue? { nil }
    func save(_ checkpoint: JsonValue) async throws(BroError) { fatalError("not used") }
}

/// '<code>' then ' <key>=<value>' per param: strings as they are, other values as canonical JSON without its final
/// newline.
private struct SealRenderer: MessageRenderer {
    func render(_ message: BroMessage) -> String {
        message.code.rawValue + message.params.map { p in
            var json = JsonValue.encodeCanonical(p.value)
            if json.last == UInt8(ascii: "\n") { json.removeLast() }
            return " " + p.key + "=" + (p.value.string ?? String(decoding: json, as: UTF8.self))
        }.joined()
    }
}

/// Every key of `expected` is in `actual` with the same value; arrays have the same length and match element by element.
private func subset(_ expected: JsonValue, _ actual: JsonValue?, _ path: String) throws {
    switch expected {
    case .object(let members):
        guard actual?.members != nil else { throw FixtureError("\(path): expected an object, got \(String(describing: actual))") }
        for m in members { try subset(m.value, actual?[m.key], path + "." + m.key) }
    case .array(let items):
        guard let b = actual?.array, b.count == items.count else { throw FixtureError("\(path): expected \(expected), got \(String(describing: actual))") }
        for (i, item) in items.enumerated() { try subset(item, b[i], "\(path)[\(i)]") }
    default:
        if let n = expected.double, let m = actual?.double { try Fixtures.same(n, m, path) } else { try Fixtures.same(expected, actual ?? .null, path) }
    }
}

/// Every file under staging (relative path → text); empty when there is no staging folder.
private func snapshot(_ fs: MemoryFileSystem, _ staging: String) throws -> [String: String] {
    guard fs.exists(staging) else { return [:] }
    var result: [String: String] = [:]
    for f in try FileTree.files(fs, root: staging) where f.utf8.last != UInt8(ascii: "/") { result[f] = String(decoding: try fs.read(staging + "/" + f), as: UTF8.self) }
    return result
}

/// shared/fixtures/engine/seal-steps.cases.json: SealStep alone, on a file tree in memory (a disk and a cache, failures, a
/// cancel on an operation), a fixed clock and the engine harnesses' fake renderer. The format is in
/// claudeDocs/tools/rebuild-phase3/gen-seal-cases.py.
struct SealStepTests {
    @Test func theSealCasesPass() async throws {
        var failures: [String] = []
        let doc = try Fixtures.json("engine/seal-steps.cases.json")
        let cases = doc["cases"]?.array ?? []
        #expect(!cases.isEmpty)
        for c in cases {
            let id = c["id"]?.string ?? "?"
            do {
                try await run(c["given"] ?? .null, c["expect"] ?? .null)
            } catch {
                failures.append("\(id): \(error)")
            }
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    private func run(_ given: JsonValue, _ expect: JsonValue) async throws {
        let step = SealStep()
        let fs = MemoryFileSystem()
        let fakes = SealFakes(given, fs)
        let staging = (given["libraries"]?["lib"]?["path"]?.string ?? "/lib") + "/.bromelia/staging/" + jobIdText
        let jobDirectory = "/data/jobs/" + jobIdText
        func putStaging(_ rel: String, _ content: JsonValue) {
            if rel.utf8.last == UInt8(ascii: "/") {
                fs.putFolder(staging + "/" + String(decoding: rel.utf8.dropLast(), as: UTF8.self)) // an empty folder (by bytes: "a\u{600}/")
            } else if let text = content.string {
                fs.put(staging + "/" + rel, Array(text.utf8))
            } else {
                fs.put(staging + "/" + rel, Array((content["disk"]?.string ?? "").utf8))
                fs.setCached(staging + "/" + rel, Array((content["cache"]?.string ?? "").utf8))
            }
        }
        for (rel, content) in given["staging"]?.members ?? [] { putStaging(rel, content) }
        for (name, text) in given["job"]?.members ?? [] { fs.put(jobDirectory + "/" + name, Array((text.string ?? "").utf8)) }
        for f in given["fails"]?.array ?? [] {
            fs.fail(f["op"]?.string ?? "", f["path"]?.string ?? "", BroError(f["error"]?["code"]?.string ?? "", f["error"]?["params"]?.members ?? []))
        }
        let cancel = CancellationSource()
        if let at = given["cancelAt"] {
            let op = at["op"]?.string, path = at["path"]?.string
            fs.onOp = { o, p in if o == op && p == path { cancel.cancel() } }
        }
        let outputs = (given["outputs"]?.array ?? []).map {
            StepOutput(kind: StepKind(rawValue: $0["kind"]?.string ?? "")!, data: $0["data"]!, readErrors: Int($0["readErrors"]?.int ?? 0))
        }
        let r = given["request"]!
        let request = JobRequest(kind: r["kind"]?.string.flatMap(JobKind.init(rawValue:)) ?? .videoDisc, title: r["title"]?.string ?? "",
                                 automatic: r["automatic"]?.bool ?? false, driveId: r["driveId"]?.string, mediaGeneration: r["mediaGeneration"]?.int,
                                 sessionId: nil, options: r["options"], details: r["details"])
        let job = JobSnapshot(id: Id(jobIdText), request: request, plan: nil, outputs: outputs, startedAt: Instant.parse(given["startedAt"]?.string ?? ""))
        let unused = UnusedTools()
        let context = StepContext(job: job, cancel: cancel.token, progress: fakes, log: fakes, decisions: fakes, tools: ToolSet(makemkv: unused, matroska: unused),
                                  fs: fs, clock: fakes, drives: unused, library: fakes, catalog: unused, config: Config(document: .object([]), issues: []),
                                  driveLookup: unused, makemkvSetup: unused, space: unused, checkpoint: fakes, jobDirectory: jobDirectory,
                                  messages: SealRenderer(), appVersion: given["appVersion"]?.string ?? "")

        if let resources = expect["resources"] {
            try Fixtures.same(resources["libraryWrites"], .array(step.resources(job).libraryWrites.map { .string($0) }), "resources.libraryWrites")
        }
        if let resumes = expect["resumes"] { try Fixtures.same(resumes["resumability"]?.string, step.resumability.rawValue, "resumability") }

        let runs = Int(given["runs"]?.int ?? 1)
        var first: [String: String] = [:]
        var report: StepReport?
        var error: BroError?
        for n in 1...runs {
            if n == runs {
                if n > 1 { for (rel, content) in given["between"]?.members ?? [] { putStaging(rel, content) } }
                fs.clearRecorded()
                fakes.clear()
                fs.recording = true
            }
            report = nil
            error = nil
            do throws(BroError) {
                report = try await step.run(context)
                error = report?.error
            } catch let e {
                error = e
            }
            fs.recording = false
            if n == 1 && runs > 1 {
                if let error { throw FixtureError("the first run failed: \(error.code) \(JsonValue.object(error.params))") }
                first = try snapshot(fs, staging)
            }
        }

        if let e = expect["error"] {
            try Fixtures.same(e["code"]?.string, error?.code, "error")
            try Fixtures.same(e["params"] ?? .object([]), JsonValue.object(error?.params ?? []), "error params")
        } else if let error {
            throw FixtureError("unexpected error \(error.code) \(JsonValue.object(error.params))")
        }
        try Fixtures.same(expect["skipped"]?.bool ?? false, report?.skipped ?? false, "skipped")
        if let data = expect["data"] { try subset(data, report?.data, "data") }
        if let log = expect["log"]?.array {
            let got = fakes.logged
            try Fixtures.same(log.count, got.count, "log length (\(got.map(\.code.rawValue).joined(separator: ", ")))")
            for (i, e) in log.enumerated() {
                try Fixtures.same(e["code"]?.string, got[i].code.rawValue, "log[\(i)]")
                try Fixtures.same(e["params"] ?? .object([]), JsonValue.object(got[i].params), "log[\(i)] params")
                if let sev = e["severity"]?.string { try Fixtures.same(sev, got[i].severity.rawValue, "log[\(i)] severity") }
            }
        }
        if let progress = expect["progress"]?.array {
            try Fixtures.same(progress.count, fakes.progressed.count, "progress length")
            try subset(.array(progress), .array(fakes.progressed), "progress")
        }
        if let ops = expect["fs"]?.array { try Fixtures.same(ops.map { $0.string ?? "" }.joined(separator: "\n"), fs.ops.joined(separator: "\n"), "fs") }
        let reads = fs.reads
        for (rel, n) in expect["reads"]?.members ?? [] { try Fixtures.same(Int(n.int ?? -1), reads[staging + "/" + rel] ?? 0, "reads of " + rel) }
        try Fixtures.same(0, fs.openStreams, "streams left open") // every case: a handle left open blocks Commit's move on Windows
        let after = try snapshot(fs, staging)
        if let tree = expect["tree"] {
            let files = fs.exists(staging) ? try FileTree.files(fs, root: staging) : []
            try Fixtures.same(tree, .array(files.map { .string($0) }), "tree")
        }
        for (rel, text) in expect["contents"]?.members ?? [] { try Fixtures.same(text.string, after[rel], "contents of " + rel) }
        if let record = expect["record"]?.string {
            let name = report?.data["record"]?.string ?? ""
            try Fixtures.same(record, after[name], "record " + name)
        }
        if expect["sameAsFirst"]?.bool == true {
            let order: (String, String) -> Bool = { $0.utf8.lexicographicallyPrecedes($1.utf8) }
            try Fixtures.same(first.keys.sorted(by: order).joined(separator: "\n"), after.keys.sorted(by: order).joined(separator: "\n"), "the files after the rerun")
            for (rel, text) in first { try Fixtures.same(text, after[rel], "the rerun's " + rel) }
        }
    }
}
