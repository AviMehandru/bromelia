import BroDomain
import BroEngine
import BroFoundation
import BroPorts
import BroTestSupport
import Foundation
import Testing

private let jobIdText = "00000000-0000-4000-8000-000000000001"

/// The log, the library, the clock, the checkpointer and the drive lookup a Name case gives the step.
private final class NameFakes: ProgressReporter, JobLogger, LibraryPort, Clock, DecisionPort, Checkpointer, DriveLookup, @unchecked Sendable {
    let given: JsonValue
    let fs: MemoryFileSystem
    private let lock = NSLock()
    private var _log: [BroMessage] = []
    private var _drives: JsonValue?
    private var _stored: JsonValue?
    private var _saved: JsonValue?
    private var _saves = 0

    init(_ given: JsonValue, _ fs: MemoryFileSystem) {
        self.given = given
        self.fs = fs
        _drives = given["drives"]
        _stored = given["checkpoint"]
    }

    var logged: [BroMessage] { lock.withLock { _log } }
    /// What the checkpointer holds: an attempt sees what the one before it saved.
    var stored: JsonValue? { lock.withLock { _stored } }
    var saves: Int { lock.withLock { _saves } }
    var drives: JsonValue? {
        get { lock.withLock { _drives } }
        set { lock.withLock { _drives = newValue } }
    }

    /// Before an attempt: what it sees as saved is what the checkpointer holds now.
    func beginAttempt() { lock.withLock { _saved = _stored } }

    func clear() {
        lock.withLock {
            _log.removeAll()
            _saves = 0
        }
    }

    func report(_ fraction: Double, operation: BroMessage, current: String?) {}

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

    func commit(_ unit: UnitRecord, intent: CommitIntent, files: [UnitFile]) async throws(BroError) -> CommitResult { throw unused() }
    func quarantine(_ unit: UnitRecord, intent: CommitIntent, files: [UnitFile], tag: String) async throws(BroError) -> CommitResult { throw unused() }

    func now() -> Instant { Instant.parse(given["now"]?.string ?? "") ?? Instant(unixMilliseconds: 0) }
    func monotonic() -> Duration { fatalError("not used") }
    func sleep(_ duration: Duration, cancel: CancellationToken) async throws(BroError) { throw unused() }
    func timer(_ schedule: TimerSchedule, handler: @escaping @Sendable () -> Void) -> any TimerHandle { fatalError("not used") }

    /// UTC plus the case's utcOffsetMinutes; milliseconds cut.
    func localTime(_ instant: Instant) -> LocalTime {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = TimeZone(secondsFromGMT: Int(given["utcOffsetMinutes"]?.int ?? 0) * 60)!
        let ms = instant.unixMilliseconds
        let seconds = ms >= 0 ? ms / 1000 : (ms - 999) / 1000
        let c = calendar.dateComponents([.year, .month, .day, .hour, .minute, .second], from: Date(timeIntervalSince1970: Double(seconds)))
        return LocalTime(year: c.year!, month: c.month!, day: c.day!, hour: c.hour!, minute: c.minute!, second: c.second!)
    }

    func ask(_ decision: DecisionDto) async throws(BroError) -> String { throw unused() }

    /// Fixed for the attempt, as the runner's is.
    var saved: JsonValue? { lock.withLock { _saved } }

    func save(_ checkpoint: JsonValue) async throws(BroError) {
        lock.withLock {
            _stored = checkpoint
            _saves += 1
        }
    }

    func find(_ driveId: String) -> DriveLocation? {
        guard let d = drives?[driveId] else { return nil }
        return DriveLocation(makemkvIndex: nil, device: "", configId: d["configId"]?.string, generation: nil)
    }

    private func unused() -> BroError { BroError(MessageCode.wire(.internalUnexpected), [("detail", .string("not used by Name"))]) }
}

/// shared/fixtures/engine/name-steps.cases.json: NameStep alone, on one file tree in memory holding the library and the
/// job's staging folder, a clock whose local time is UTC plus an offset, a checkpointer kept between attempts, a drive
/// lookup and the configuration. The format is in claudeDocs/tools/rebuild-phase3/gen-name-cases.py.
struct NameStepTests {
    @Test func theNameCasesPass() async throws {
        var failures: [String] = []
        let doc = try Fixtures.json("engine/name-steps.cases.json")
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
        let step = NameStep()
        let fs = MemoryFileSystem()
        let fakes = NameFakes(given, fs)
        let library = given["libraries"]?["lib"]?["path"]?.string ?? "/lib"
        let staging = library + "/.bromelia/staging/" + jobIdText
        func put(_ root: String, _ entries: JsonValue?) {
            for (rel, content) in entries?.members ?? [] {
                if rel.hasSuffix("/") {
                    fs.putFolder(root + "/" + rel.dropLast()) // an empty folder
                } else {
                    fs.put(root + "/" + rel, Array((content.string ?? "").utf8))
                }
            }
        }
        put(library, given["library"])
        put(staging, given["staging"])
        for f in given["fails"]?.array ?? [] {
            fs.fail(f["op"]?.string ?? "", f["path"]?.string ?? "", BroError(f["error"]?["code"]?.string ?? "", f["error"]?["params"]?.members ?? []))
        }
        var cancel = CancellationSource()
        if given["cancelAt"]?["op"]?.string == "start" {
            cancel.cancel() // before the step runs
        } else if let at = given["cancelAt"] {
            let op = at["op"]?.string, path = at["path"]?.string, source = cancel
            fs.onOp = { o, p in if o == op && p == path { source.cancel() } }
        }
        var outputs: [StepOutput] = []
        for o in given["outputs"]?.array ?? [] {
            guard let kind = StepKind(rawValue: o["kind"]?.string ?? "") else { throw FixtureError("unknown step kind \(String(describing: o["kind"]))") }
            outputs.append(StepOutput(kind: kind, data: o["data"] ?? .null, readErrors: 0))
        }
        let r = given["request"] ?? .object([])
        let request = JobRequest(kind: r["kind"]?.string.flatMap(JobKind.init(rawValue:)) ?? .videoDisc, title: r["title"]?.string ?? "",
                                 automatic: r["automatic"]?.bool ?? false, driveId: r["driveId"]?.string, mediaGeneration: r["mediaGeneration"]?.int,
                                 sessionId: nil, options: r["options"], details: r["details"])
        let job = JobSnapshot(id: Id(jobIdText), request: request, plan: nil, outputs: outputs, startedAt: given["startedAt"]?.string.flatMap(Instant.parse))
        let config = Config(document: given["config"] ?? .object([]), issues: [])
        let unused = UnusedTools()

        if let resources = expect["resources"] {
            try Fixtures.same(resources["libraryWrites"], .array(step.resources(job).libraryWrites.map { .string($0) }), "resources.libraryWrites")
        }
        if let resumes = expect["resumes"] { try Fixtures.same(resumes["resumability"]?.string, step.resumability.rawValue, "resumability") }

        let attempts = Int(given["attempts"]?.int ?? 1)
        var report: StepReport?
        var error: BroError?
        for attempt in 1...max(1, attempts) {
            if attempt > 1 {
                fs.clearFails()
                fs.onOp = nil
                cancel = CancellationSource()
            }
            if attempt == attempts {
                if attempt > 1, let between = given["between"] {
                    put(library, between["library"])
                    put(staging, between["staging"])
                    if let drives = between["drives"] { fakes.drives = drives }
                }
                fs.clearRecorded()
                fakes.clear()
                fs.recording = true
            }
            fakes.beginAttempt()
            let context = StepContext(job: job, cancel: cancel.token, progress: fakes, log: fakes, decisions: fakes,
                                      tools: ToolSet(makemkv: unused, matroska: unused), fs: fs, clock: fakes, drives: unused, library: fakes,
                                      catalog: unused, config: config, driveLookup: fakes, makemkvSetup: unused, space: unused, checkpoint: fakes,
                                      jobDirectory: "/data/jobs/" + jobIdText, messages: NameRenderer(), appVersion: "test")
            report = nil
            error = nil
            do throws(BroError) {
                report = try await step.run(context)
                error = report?.error
            } catch let e {
                error = e
            }
            fs.recording = false
            if attempt == 1 && attempts > 1 {
                if let fe = expect["firstError"] {
                    try sameError(fe, error, "the first attempt's error")
                } else if let error {
                    throw FixtureError("the first attempt failed: \(error.code) \(JsonValue.object(error.params))")
                }
            }
        }

        if let e = expect["error"] {
            try sameError(e, error, "error")
        } else if let error {
            throw FixtureError("unexpected error \(error.code) \(JsonValue.object(error.params))")
        }
        try Fixtures.same(expect["skipped"]?.bool ?? false, report?.skipped ?? false, "skipped")
        if let data = expect["data"] { try Fixtures.same(data, report?.data ?? .null, "data") }
        if let log = expect["log"]?.array {
            let got = fakes.logged
            try Fixtures.same(log.count, got.count, "log length (\(got.map(\.code.rawValue).joined(separator: ", ")))")
            for (i, e) in log.enumerated() {
                try Fixtures.same(e["code"]?.string, got[i].code.rawValue, "log[\(i)]")
                try Fixtures.same(e["params"] ?? .object([]), JsonValue.object(got[i].params), "log[\(i)] params")
            }
        }
        if let ops = expect["fs"]?.array { try Fixtures.same(ops.map { $0.string ?? "" }.joined(separator: "\n"), fs.ops.joined(separator: "\n"), "fs") }
        if let tree = expect["tree"] {
            let files = fs.exists(staging) ? try FileTree.files(fs, root: staging) : []
            try Fixtures.same(tree, .array(files.map { .string($0) }), "tree")
        }
        if expect.members?.contains(where: { $0.key == "checkpoint" }) == true {
            try Fixtures.same(expect["checkpoint"] ?? .null, fakes.stored ?? .null, "checkpoint")
        }
        if let saves = expect["saves"]?.int { try Fixtures.same(Int(saves), fakes.saves, "saves") }
    }

    private func sameError(_ expected: JsonValue, _ error: BroError?, _ what: String) throws {
        try Fixtures.same(expected["code"]?.string, error?.code, what)
        try Fixtures.same(expected["params"] ?? .object([]), JsonValue.object(error?.params ?? []), what + " params")
    }
}

/// Name renders nothing; the code is enough.
private struct NameRenderer: MessageRenderer {
    func render(_ message: BroMessage) -> String { message.code.rawValue }
}
