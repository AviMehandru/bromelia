import BroDomain
import BroEngine
import BroFoundation
import BroPorts
import BroTestSupport
import Foundation
import Testing

private let commitJobId = "00000000-0000-4000-8000-000000000001"

/// The progress, the log, the library port and the clock a Commit case gives the step.
private final class CommitFakes: ProgressReporter, JobLogger, LibraryPort, Clock, DecisionPort, Checkpointer, @unchecked Sendable {
    let given: JsonValue
    private let lock = NSLock()
    private var _log: [BroMessage] = []
    private var _progress: [JsonValue] = []
    private var _calls: [JsonValue] = []
    private var _stagingAsked = 0
    /// Set when the case cancels the job as the port is called.
    var cancelOnCall: CancellationSource?

    init(_ given: JsonValue) { self.given = given }

    var logged: [BroMessage] { lock.withLock { _log } }
    var progressed: [JsonValue] { lock.withLock { _progress } }
    var calls: [JsonValue] { lock.withLock { _calls } }
    var stagingAsked: Int { lock.withLock { _stagingAsked } }

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
        lock.withLock { _stagingAsked += 1 }
        return path
    }

    func commit(_ unit: UnitRecord, intent: CommitIntent, files: [UnitFile], cancel: CancellationToken) async throws(BroError) -> CommitResult {
        try called("commit", unit, intent, files, nil, cancel)
    }

    func quarantine(_ unit: UnitRecord, intent: CommitIntent, files: [UnitFile], tag: String, cancel: CancellationToken) async throws(BroError) -> CommitResult {
        try called("quarantine", unit, intent, files, tag, cancel)
    }

    private func called(_ op: String, _ unit: UnitRecord, _ intent: CommitIntent, _ files: [UnitFile], _ tag: String?,
                        _ cancel: CancellationToken) throws(BroError) -> CommitResult {
        var call: [(key: String, value: JsonValue)] = [("op", .string(op))]
        if let tag { call.append(("tag", .string(tag))) }
        call.append(("unit", unitJson(unit)))
        call.append(("intent", intentJson(intent)))
        call.append(("files", .array(files.map { f in
            .array([.string(f.path), .integer(f.size), .string(f.sha256), .string(f.role), f.title.map { .integer(Int64($0)) } ?? .null])
        })))
        lock.withLock { _calls.append(.object(call)) }
        cancelOnCall?.cancel()
        // LibraryService ends its wait for the unit so when the token it was given is cancelled.
        if cancel.isCancelled { throw BroError(MessageCode.wire(.jobCancelled)) }
        let port = given["port"]
        if let e = port?["error"] { throw BroError(e["code"]?.string ?? "", e["params"]?.members ?? []) }
        let bytes = Array(intent.destination.utf8)
        let cut = bytes.lastIndex(of: UInt8(ascii: "/")) ?? 0
        let parent = String(decoding: bytes[..<cut], as: UTF8.self), last = String(decoding: bytes[(cut + 1)...], as: UTF8.self)
        let destination = port?["destination"]?.string ?? (tag.map { parent + "/" + last + " [" + $0 + "]" } ?? intent.destination)
        return CommitResult(unitId: unit.id, destination: destination, files: files, merged: port?["merged"]?.bool ?? (tag == nil && intent.merge))
    }

    func now() -> Instant { Instant.parse(given["now"]?.string ?? "")! }
    func monotonic() -> Duration { fatalError("not used") }
    func sleep(_ duration: Duration, cancel: CancellationToken) async throws(BroError) { fatalError("not used") }
    func timer(_ schedule: TimerSchedule, handler: @escaping @Sendable () -> Void) -> any TimerHandle { fatalError("not used") }
    func localTime(_ instant: Instant) -> LocalTime { fatalError("not used") }

    func ask(_ decision: DecisionDto) async throws(BroError) -> String { fatalError("not used") }
    var saved: JsonValue? { nil }
    func save(_ checkpoint: JsonValue) async throws(BroError) { fatalError("not used") }
}

private func n(_ v: Int?) -> JsonValue { v.map { .integer(Int64($0)) } ?? .null }

/// UnitRecord's fields in its order; enums by their wire names, instants formatted.
private func unitJson(_ u: UnitRecord) -> JsonValue {
    .object([("id", .string(u.id.value)), ("libraryId", .string(u.libraryId)), ("physicalDiscId", u.physicalDiscId.map { .string($0.value) } ?? .null),
             ("jobId", u.jobId.map { .string($0.value) } ?? .null), ("path", .string(u.path)), ("recordFile", .string(u.recordFile)),
             ("recordVersion", .integer(Int64(u.recordVersion))), ("state", .string(u.state.rawValue)), ("status", .string(u.status.rawValue)),
             ("name", .string(u.name)), ("kind", .string(u.kind.rawValue)), ("format", .string(u.format)), ("formatCode", .string(u.formatCode)),
             ("encrypted", .bool(u.encrypted)), ("fingerprint", u.fingerprint.map { .string($0) } ?? .null), ("label", .string(u.label)),
             ("season", n(u.season)), ("part", n(u.part)), ("volume", n(u.volume)), ("disc", n(u.disc)), ("makemkvVersion", .string(u.makemkvVersion)),
             ("bytes", .integer(u.bytes)), ("fileCount", .integer(Int64(u.fileCount))), ("attempts", .integer(Int64(u.attempts))),
             ("createdAt", .string(Instant.format(u.createdAt))), ("committedAt", u.committedAt.map { .string(Instant.format($0)) } ?? .null)])
}

private func intentJson(_ i: CommitIntent) -> JsonValue {
    .object([("unitId", .string(i.unitId.value)), ("jobId", .string(i.jobId.value)), ("staging", .string(i.staging)),
             ("destination", .string(i.destination)), ("merge", .bool(i.merge)), ("quarantine", .bool(i.quarantine)),
             ("createdAt", .string(Instant.format(i.createdAt))),
             ("items", .array(i.items.map { x in
                 .array([.integer(Int64(x.seq)), .string(x.fromPath), .string(x.toPath), x.sha256.map { .string($0) } ?? .null, .bool(x.isDir), .bool(x.moved)])
             }))])
}

/// shared/fixtures/engine/commit-steps.cases.json: CommitStep (or NotifyStep) alone, with a fake library port that
/// records what it is asked to commit, a fixed clock, a log and progress. The format is in
/// claudeDocs/tools/rebuild-phase3/gen-commit-cases.py.
struct CommitStepTests {
    @Test func theCommitCasesPass() async throws {
        var failures: [String] = []
        let doc = try Fixtures.json("engine/commit-steps.cases.json")
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
        let step: any Step = given["step"]?.string == "notify" ? NotifyStep() : CommitStep()
        let fakes = CommitFakes(given)
        let cancel = CancellationSource()
        switch given["cancelAt"]?["op"]?.string {
        case "start": cancel.cancel()
        case "commit": fakes.cancelOnCall = cancel
        default: break
        }
        var outputs: [StepOutput] = []
        for o in given["outputs"]?.array ?? [] {
            guard let kind = o["kind"]?.string.flatMap(StepKind.init(rawValue:)) else { throw FixtureError("unknown step kind \(o["kind"] ?? .null)") }
            outputs.append(StepOutput(kind: kind, data: o["data"] ?? .null, readErrors: Int(o["readErrors"]?.int ?? 0)))
        }
        let r = given["request"] ?? .object([])
        let request = JobRequest(kind: r["kind"]?.string.flatMap(JobKind.init(rawValue:)) ?? .videoDisc, title: r["title"]?.string ?? "",
                                 automatic: r["automatic"]?.bool ?? false, driveId: r["driveId"]?.string, mediaGeneration: r["mediaGeneration"]?.int,
                                 sessionId: nil, options: r["options"], details: r["details"])
        let job = JobSnapshot(id: Id(commitJobId), request: request, plan: nil, outputs: outputs, startedAt: nil)
        let unused = UnusedTools()
        let context = StepContext(job: job, cancel: cancel.token, progress: fakes, log: fakes, decisions: fakes, tools: ToolSet(makemkv: unused, matroska: unused),
                                  fs: MemoryFileSystem(), clock: fakes, drives: unused, library: fakes, catalog: unused,
                                  config: Config(document: .object([]), issues: []), driveLookup: unused, makemkvSetup: unused, space: unused,
                                  checkpoint: fakes, jobDirectory: "/data/jobs/" + commitJobId, messages: UnusedRenderer(), appVersion: "test")

        if let resources = expect["resources"] {
            try Fixtures.same(resources["libraryWrites"], .array(step.resources(job).libraryWrites.map { .string($0) }), "resources.libraryWrites")
        }
        if let resumes = expect["resumes"] { try Fixtures.same(resumes["resumability"]?.string, step.resumability.rawValue, "resumability") }

        var report: StepReport?
        var error: BroError?
        do throws(BroError) {
            report = try await step.run(context)
            error = report?.error
        } catch let e {
            error = e
        }

        if let e = expect["error"] {
            try Fixtures.same(e["code"]?.string, error?.code, "error")
            try Fixtures.same(e["params"] ?? .object([]), JsonValue.object(error?.params ?? []), "error params")
        } else if let error {
            throw FixtureError("unexpected error \(error.code) \(JsonValue.object(error.params))")
        }
        try Fixtures.same(expect["skipped"]?.bool ?? false, report?.skipped ?? false, "skipped")
        try Fixtures.same(expect["quarantined"]?.bool ?? false, report?.quarantined ?? false, "quarantined")
        if let data = expect["data"] { try Fixtures.same(data, report?.data ?? .null, "data") }
        if let log = expect["log"]?.array {
            let got = fakes.logged
            try Fixtures.same(log.count, got.count, "log length (\(got.map(\.code.rawValue).joined(separator: ", ")))")
            for (i, e) in log.enumerated() {
                try Fixtures.same(e["code"]?.string, got[i].code.rawValue, "log[\(i)]")
                try Fixtures.same(e["params"] ?? .object([]), JsonValue.object(got[i].params), "log[\(i)] params")
                try Fixtures.same(e["severity"]?.string, got[i].severity.rawValue, "log[\(i)] severity")
            }
        }
        if let progress = expect["progress"]?.array {
            let got = fakes.progressed
            try Fixtures.same(progress.count, got.count, "progress length")
            for (i, p) in progress.enumerated() {
                let (want, have) = (p.array ?? [], got[i].array ?? [])
                try Fixtures.same(want.first?.double, have.first?.double, "progress[\(i)] fraction")
                try Fixtures.same(JsonValue.array(Array(want.dropFirst())), JsonValue.array(Array(have.dropFirst())), "progress[\(i)]")
            }
        }
        if let calls = expect["calls"] { try Fixtures.same(calls, .array(fakes.calls), "calls") }
        if let asked = expect["stagingAsked"] { try Fixtures.same(asked.int, Int64(fakes.stagingAsked), "stagingAsked") }
    }
}

/// Commit renders nothing.
private struct UnusedRenderer: MessageRenderer {
    func render(_ message: BroMessage) -> String { fatalError("not used") }
}
