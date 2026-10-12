import BroDomain
import BroEngine
import BroFoundation
import BroPorts
import BroTestSupport
import Foundation
import Testing

/// The ports a case gives, recording every call.
private final class StepFakes: Makemkv, DriveControl, DriveLookup, MakemkvSetup, ProgressReporter, JobLogger, @unchecked Sendable {
    let given: JsonValue
    private let lock = NSLock()
    private var _calls: [String] = []
    private var _log: [BroMessage] = []
    private var _progress: [JsonValue] = []

    init(_ given: JsonValue) { self.given = given }

    var calls: [String] { lock.withLock { _calls } }
    var logged: [BroMessage] { lock.withLock { _log } }
    var progressed: [JsonValue] { lock.withLock { _progress } }

    private func call(_ text: String) { lock.withLock { _calls.append(text) } }

    func find(_ driveId: String) -> DriveLocation? {
        guard let d = given["drives"]?[driveId] else { return nil }
        return DriveLocation(makemkvIndex: d["makemkvIndex"]?.int.map { Int($0) }, device: d["device"]?.string ?? "", configId: d["configId"]?.string,
                             generation: d["generation"]?.int, label: d["label"]?.string ?? "", flags: d["flags"]?.int.map { DiscFlags(raw: Int($0)) })
    }

    func invocation(_ profile: EffectiveProfile, selectionOverride: String?, jobDirectory: String) throws(BroError) -> MakemkvInvocation {
        call("invocation \(selectionOverride ?? "-") minLength=\(profile.profile["makemkv"]?["minLengthSeconds"]?.int.map { String($0) } ?? "null")")
        return MakemkvInvocation(settings: MakemkvRunSettings(settings: [:], dataDir: "", workDirectory: jobDirectory), options: MakemkvOptions())
    }

    func listing(_ source: MakemkvSource, invocation: MakemkvInvocation, sink: any RunSink, cancel: CancellationToken) async throws(BroError) -> ListingRun {
        switch source {
        case .drive(let index, _): call("listing disc:\(index)")
        case .iso(let path): call("listing iso:" + path)
        case .file(let path): call("listing file:" + path)
        }
        let m = given["makemkv"] ?? .object([])
        let text = m["listing"].map(Fixtures.generatedListing) ?? (m["robot"]?.array ?? []).map { $0.string ?? "" }.joined(separator: "\n")
        var builder = ListingBuilder()
        for line in text.split(separator: "\n", omittingEmptySubsequences: false) {
            if let e = Robot.parseLine(String(line)) {
                sink.event(e)
                builder.feed(e)
            }
        }
        let o = m["outcome"] ?? .object([])
        func message(_ j: JsonValue?, _ severity: Severity) -> BroMessage? {
            guard let j, let code = j["code"]?.string.flatMap(MessageCode.init(rawValue:)) else { return nil }
            return BroMessage(code, j["params"]?.members ?? [], severity: severity)
        }
        let outcome = RunOutcome(saved: nil, failed: nil, exitCode: Int(o["exitCode"]?.int ?? 0), firstError: nil, spaceWarning: nil, readErrors: [],
                                 driveMismatch: nil, debugLog: nil, status: o["status"]?.string.flatMap(StatusWord.init(rawValue:)) ?? .success,
                                 error: message(o["error"], .error), produced: [])
        let notice: MakemkvNotice? = switch m["notice"]?.string {
        case "keyExpired": .keyExpired
        case "evaluationNotStarted": .evaluationNotStarted
        case "versionTooOld": .versionTooOld
        case "libreDriveRequired": .libreDriveRequired
        default: nil
        }
        return ListingRun(listing: builder.build(),
                          run: MakemkvRun(outcome: outcome, notice: notice, libreDrive: m["libreDrive"]?.string,
                                          transcriptProblem: message(m["transcriptProblem"], .warning), settingsProblem: message(m["settingsProblem"], .warning)))
    }

    func scanDrives(_ cancel: CancellationToken) async throws(BroError) -> [MakemkvDrive] { fatalError("not used") }
    func rip(_ source: MakemkvSource, title: String, destination: String, invocation: MakemkvInvocation, sink: any RunSink,
             cancel: CancellationToken) async throws(BroError) -> MakemkvRun { fatalError("not used") }
    func backup(_ source: MakemkvSource, decrypt: Bool, destination: String, invocation: MakemkvInvocation, sink: any RunSink,
                cancel: CancellationToken) async throws(BroError) -> MakemkvRun { fatalError("not used") }

    func eject(_ device: String) async throws(BroError) {
        call("eject " + device)
        if let e = given["eject"] { throw BroError(e["code"]?.string ?? "", e["params"]?.members ?? []) }
    }

    func closeTray(_ device: String) async throws(BroError) { fatalError("not used") }

    func waitForMount(_ device: String, timeout: Duration, cancel: CancellationToken) async -> String? {
        call("waitForMount \(device) \(Int(timeout.seconds))")
        return given["mount"]?.string
    }

    func probeContent(_ device: String) -> DiscContent { fatalError("not used") }
    func openRaw(_ device: String) throws(BroError) -> any SectorReader { fatalError("not used") }

    func report(_ fraction: Double, operation: BroMessage, current: String?) {
        var item: [JsonValue] = [.number(fraction), .string(operation.code.rawValue)]
        if let current { item.append(.string(current)) }
        lock.withLock { _progress.append(.array(item)) }
    }

    func log(_ message: BroMessage) { lock.withLock { _log.append(message) } }
}

/// The steps of this file never ask.
private struct NoDecisions: DecisionPort {
    func ask(_ decision: DecisionDto) async throws(BroError) -> String { fatalError("not used") }
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

private func noticeName(_ n: MakemkvNotice?) -> String? {
    switch n {
    case .keyExpired?: "keyExpired"
    case .evaluationNotStarted?: "evaluationNotStarted"
    case .versionTooOld?: "versionTooOld"
    case .libreDriveRequired?: "libreDriveRequired"
    case .libreDrive?: "libreDrive"
    case nil: nil
    }
}

/// shared/fixtures/engine/video-steps.cases.json: each step alone, with fake ports.
struct VideoStepsTests {
    static let steps: [StepKind: any Step] = Dictionary(uniqueKeysWithValues: ([AwaitMediaStep(), ProbeStep(), ReconcileStep(), IdentifyStep(), ReleaseStep()]
        as [any Step]).map { ($0.kind, $0) })

    @Test func theSharedCasesPass() async throws {
        var failures: [String] = []
        let doc = try Fixtures.json("engine/video-steps.cases.json")
        for c in doc["cases"]?.array ?? [] {
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
        let step = Self.steps[StepKind(rawValue: given["step"]?.string ?? "")!]!
        let fakes = StepFakes(given)
        let config = try ConfigCodec.decode(JsonValue.encodeCanonical(given["config"] ?? .object([("version", .integer(3))])))
        let outputs = (given["outputs"]?.array ?? []).map { StepOutput(kind: StepKind(rawValue: $0["kind"]?.string ?? "")!, data: $0["data"]!) }
        let r = given["request"]!
        let request = JobRequest(kind: r["kind"]?.string.flatMap(JobKind.init(rawValue:)) ?? .videoDisc, title: r["title"]?.string ?? "",
                                 automatic: r["automatic"]?.bool ?? false, driveId: r["driveId"]?.string, mediaGeneration: r["mediaGeneration"]?.int,
                                 sessionId: r["sessionId"]?.string.map(Id.init), options: r["options"], details: r["details"])
        let job = JobSnapshot(id: Id("00000000-0000-4000-8000-000000000001"), request: request, plan: nil, outputs: outputs)
        let unused = UnusedTools()
        let cancel = CancellationSource()
        let context = StepContext(job: job, cancel: cancel.token, progress: fakes, log: fakes, decisions: NoDecisions(), tools: ToolSet(makemkv: fakes, matroska: unused),
                                  fs: MemoryFileSystem(), clock: ManualClock(), drives: fakes, library: unused, catalog: unused, config: config,
                                  driveLookup: fakes, makemkvSetup: fakes, jobDirectory: "/data/jobs/" + job.id.value)

        if let resources = expect["resources"] {
            try Fixtures.same(resources["drives"], .array(step.resources(job).drives.map { .string($0) }), "resources.drives")
        }
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
        if let notice = expect["notice"] { try Fixtures.same(notice.string, noticeName(report?.notice), "notice") }
        try Fixtures.same(expect["skipped"]?.bool ?? false, report?.skipped ?? false, "skipped")
        if let data = expect["data"] { try subset(data, report?.data, "data") }
        if let log = expect["log"]?.array {
            let got = fakes.logged
            try Fixtures.same(log.count, got.count, "log length (\(got.map(\.code.rawValue).joined(separator: ", ")))")
            for (i, e) in log.enumerated() {
                try Fixtures.same(e.string ?? e["code"]?.string, got[i].code.rawValue, "log[\(i)]")
                if e.members != nil {
                    try Fixtures.same(e["params"] ?? .object([]), JsonValue.object(got[i].params), "log[\(i)] params")
                    if let sev = e["severity"]?.string { try Fixtures.same(sev, got[i].severity.rawValue, "log[\(i)] severity") }
                }
            }
        }
        if let calls = expect["calls"]?.array {
            try Fixtures.same(calls.map { $0.string ?? "" }.joined(separator: " | "), fakes.calls.joined(separator: " | "), "calls")
        }
        if let progress = expect["progress"] { try subset(progress, .array(fakes.progressed), "progress") }
    }
}
