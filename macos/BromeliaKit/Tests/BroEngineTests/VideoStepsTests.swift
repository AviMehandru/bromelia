import BroDomain
import BroEngine
import BroFoundation
import BroPorts
import BroTestSupport
import Foundation
import Testing

/// The ports a case gives, recording every call.
private final class StepFakes: Makemkv, DriveControl, DriveLookup, MakemkvSetup, ProgressReporter, JobLogger, LibraryPort, CatalogQueries,
    DecisionPort, SpaceReservations, @unchecked Sendable {
    let given: JsonValue
    private let lock = NSLock()
    private var _calls: [String] = []
    private var _log: [BroMessage] = []
    private var _progress: [JsonValue] = []
    private var _question: DecisionDto?
    /// Probe's fingerprint ('archivedBefore probe').
    var fingerprint: String?

    init(_ given: JsonValue) { self.given = given }

    var calls: [String] { lock.withLock { _calls } }
    var logged: [BroMessage] { lock.withLock { _log } }
    var progressed: [JsonValue] { lock.withLock { _progress } }
    var question: DecisionDto? { lock.withLock { _question } }

    func clear() {
        lock.withLock {
            _calls = []
            _log = []
            _progress = []
        }
    }

    func require(_ libraryId: String) throws(BroError) -> LibraryHandle {
        guard let l = given["libraries"]?[libraryId] else { throw BroError(MessageCode.wire(.libraryNotFound)) }
        if let why = l["offline"] { throw BroError(why["code"]?.string ?? "", why["params"]?.members ?? []) }
        return LibraryHandle(id: libraryId, path: l["path"]?.string ?? "")
    }

    func stagingDirectory(_ libraryId: String, jobId: Id) throws(BroError) -> String { fatalError("not used") }
    func commit(_ unit: UnitRecord, intent: CommitIntent, files: [UnitFile]) async throws(BroError) -> CommitResult { fatalError("not used") }
    func quarantine(_ unit: UnitRecord, intent: CommitIntent, files: [UnitFile], tag: String) async throws(BroError) -> CommitResult {
        fatalError("not used")
    }

    func archivedBefore(_ fingerprint: String) async throws(BroError) -> [UnitRecord] {
        call("archivedBefore " + (fingerprint == self.fingerprint ? "probe" : fingerprint))
        guard fingerprint == self.fingerprint else { return [] }
        return (given["catalog"]?.array ?? []).map { u in
            UnitRecord(id: Id(u["id"]?.string ?? ""), libraryId: u["libraryId"]?.string ?? "", path: u["path"]?.string ?? "", recordFile: "",
                       recordVersion: 3, state: .committed, status: u["status"]?.string.flatMap(UnitStatus.init(rawValue:)) ?? .success, name: "",
                       kind: .movie, format: "", formatCode: "", encrypted: false, fingerprint: fingerprint, label: "", makemkvVersion: "", bytes: 0,
                       fileCount: 0, attempts: 1, createdAt: Instant.parse("2026-09-01T00:00:00.000Z")!,
                       committedAt: Instant.parse(u["committedAt"]?.string ?? ""))
        }
    }

    func ask(_ decision: DecisionDto) async throws(BroError) -> String {
        lock.withLock {
            _question = decision
            _calls.append("ask \(decision.id) \(decision.question.code.rawValue) \(decision.answers.map(\.id).joined(separator: ","))")
        }
        guard let answer = given["answer"]?.string else { throw BroError(MessageCode.wire(.jobCancelled)) }
        return answer
    }

    func reserve(_ volume: String, path: String, need: Int64, free: Int64) async throws(BroError) {
        call("reserve \(volume) \(path) \(need) \(free)")
        if let e = given["reserveFails"] { throw BroError(e["code"]?.string ?? "", e["params"]?.members ?? []) }
    }

    private func call(_ text: String) { lock.withLock { _calls.append(text) } }

    func find(_ driveId: String) -> DriveLocation? {
        guard let d = given["drives"]?[driveId] else { return nil }
        return DriveLocation(makemkvIndex: d["makemkvIndex"]?.int.map { Int($0) }, device: d["device"]?.string ?? "", configId: d["configId"]?.string,
                             generation: d["generation"]?.int, label: d["label"]?.string ?? "", flags: d["flags"]?.int.map { DiscFlags(raw: Int($0)) })
    }

    func invocation(_ config: Config, profile: EffectiveProfile, selectionOverride: String?, jobDirectory: String) throws(BroError)
        -> MakemkvInvocation {
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
                                          transcriptProblem: message(m["transcriptProblem"], .warning), settingsProblem: message(m["settingsProblem"], .warning),
                                          isolationWarning: message(m["isolationWarning"], .warning)))
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

/// shared/fixtures/engine/video-steps.cases.json and plan-steps.cases.json: each step alone, with fake ports.
struct VideoStepsTests {
    static let steps: [StepKind: any Step] = Dictionary(uniqueKeysWithValues: ([AwaitMediaStep(), ProbeStep(), ReconcileStep(), IdentifyStep(), PlanStep(),
                                                                                DecideStep(), ReleaseStep()] as [any Step]).map { ($0.kind, $0) })

    @Test func theSharedCasesPass() async throws { try await cases("engine/video-steps.cases.json") }

    @Test func thePlanCasesPass() async throws { try await cases("engine/plan-steps.cases.json") }

    private func cases(_ file: String) async throws {
        var failures: [String] = []
        let doc = try Fixtures.json(file)
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
        var outputs: [StepOutput] = []
        if let probe = given["probe"] {
            var listing: Listing?
            if let disc = probe["disc"] {
                var builder = ListingBuilder()
                for line in Fixtures.generatedListing(disc).split(separator: "\n") { if let e = Robot.parseLine(String(line)) { builder.feed(e) } }
                listing = builder.build()
            }
            fakes.fingerprint = listing.flatMap(Fingerprint.of)
            outputs.append(StepOutput(kind: .probe, data: ProbeOutput.encode(ProbeOutput(listing: listing, fingerprint: fakes.fingerprint, libreDrive: nil,
                                                                                         fromSession: false))))
        }
        outputs += (given["outputs"]?.array ?? []).map { StepOutput(kind: StepKind(rawValue: $0["kind"]?.string ?? "")!, data: $0["data"]!) }
        let fs = MemoryFileSystem()
        for f in given["files"]?.array ?? [] { try fs.createDirectory(f.string ?? "", parentsMustExist: false) }
        for v in given["volumes"]?.members ?? [] {
            fs.setVolume(v.key, VolumeInfo(id: v.value["id"]?.string ?? "", freeBytes: v.value["free"]?.int ?? 0, fsType: "apfs", caseSensitive: false))
        }
        var finishing: Decided?
        if let end = given["finishing"] {
            finishing = Decided(outcome: Outcome(rawValue: end["outcome"]?.string ?? "")!, error: end["error"].map { BroError($0["code"]?.string ?? "") })
        }
        let r = given["request"]!
        let request = JobRequest(kind: r["kind"]?.string.flatMap(JobKind.init(rawValue:)) ?? .videoDisc, title: r["title"]?.string ?? "",
                                 automatic: r["automatic"]?.bool ?? false, driveId: r["driveId"]?.string, mediaGeneration: r["mediaGeneration"]?.int,
                                 sessionId: r["sessionId"]?.string.map(Id.init), options: r["options"], details: r["details"])
        let jobId = Id("00000000-0000-4000-8000-000000000001")
        let unused = UnusedTools()
        let cancel = CancellationSource()
        func contextOf(_ job: JobSnapshot) -> StepContext {
            StepContext(job: job, cancel: cancel.token, progress: fakes, log: fakes, decisions: fakes, tools: ToolSet(makemkv: fakes, matroska: unused),
                        fs: fs, clock: ManualClock(), drives: fakes, library: fakes, catalog: fakes, config: config, driveLookup: fakes,
                        makemkvSetup: fakes, space: fakes, jobDirectory: "/data/jobs/" + job.id.value, finishing: finishing)
        }
        if given["identify"]?.bool == true {
            let identified = try await IdentifyStep().run(contextOf(JobSnapshot(id: jobId, request: request, plan: nil, outputs: outputs)))
            outputs.append(StepOutput(kind: .identify, data: identified.data))
            fakes.clear()
        }
        let job = JobSnapshot(id: jobId, request: request, plan: nil, outputs: outputs)
        let context = contextOf(job)

        if let resources = expect["resources"] {
            try Fixtures.same(resources["drives"], .array(step.resources(job).drives.map { .string($0) }), "resources.drives")
            if let writes = resources["libraryWrites"] {
                try Fixtures.same(writes, .array(step.resources(job).libraryWrites.map { .string($0) }), "resources.libraryWrites")
            }
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
        if let skipJob = expect["skipJob"] {
            try Fixtures.same(skipJob.isNull ? JsonValue.null : skipJob["code"]!, report?.skipJob.map { .string($0.code.rawValue) } ?? .null, "skipJob")
            if let sp = skipJob["params"] { try Fixtures.same(sp, JsonValue.object(report?.skipJob?.params ?? []), "skipJob params") }
        }
        if let append = expect["appendSteps"] {
            try Fixtures.same(append, .array((report?.appendSteps ?? []).map { .string($0.rawValue) }), "appendSteps")
        }
        if let plan = expect["plan"] {
            try subset(plan, report?.plan.map { p in .object([("mode", p.mode.map { .string($0) } ?? .null), ("profileId", p.profileId.map { .string($0) } ?? .null),
                                                              ("libraryId", p.libraryId.map { .string($0) } ?? .null)]) }, "plan")
        }
        if let question = expect["question"] {
            try Fixtures.same(question["code"], fakes.question.map { .string($0.question.code.rawValue) } ?? .null, "question")
            try Fixtures.same(question["params"] ?? .object([]), JsonValue.object(fakes.question?.question.params ?? []), "question params")
        }
    }
}
