import BroDomain
import BroEngine
import BroFoundation
import BroPorts
import BroTestSupport
import Foundation
import Testing

/// The ports a case gives, recording every call.
private final class StepFakes: Makemkv, Matroska, DriveControl, DriveLookup, MakemkvSetup, ProgressReporter, JobLogger, LibraryPort, CatalogQueries,
    DecisionPort, SpaceReservations, Checkpointer, @unchecked Sendable {
    let given: JsonValue
    private let lock = NSLock()
    private var _calls: [String] = []
    private var _log: [BroMessage] = []
    private var _progress: [JsonValue] = []
    private var _question: DecisionDto?
    /// Probe's fingerprint ('archivedBefore probe').
    var fingerprint: String?
    let fs = MemoryFileSystem()
    let cancel = CancellationSource()
    /// makemkv.runs not taken yet.
    private var runs: [JsonValue]

    init(_ given: JsonValue) {
        self.given = given
        runs = given["makemkv"]?["runs"]?.array ?? []
    }

    /// A fingerprint "probe" stands for Probe's, both ways.
    private func probed(_ checkpoint: JsonValue, toProbe: Bool) -> JsonValue {
        guard let members = checkpoint.members else { return checkpoint }
        return .object(members.map { m in
            guard m.key == "fingerprint", let f = m.value.string, toProbe ? f == fingerprint : f == "probe" else { return m }
            return (m.key, toProbe ? .string("probe") : fingerprint.map { .string($0) } ?? .null)
        })
    }

    var saved: JsonValue? { given["checkpoint"].map { probed($0, toProbe: false) } }

    func save(_ checkpoint: JsonValue) async throws(BroError) { call("checkpoint " + oneLine(probed(checkpoint, toProbe: true))) }

    private func describe(_ source: MakemkvSource) -> String {
        switch source {
        case .drive(let index, _): "disc:\(index)"
        case .iso(let path): "iso:" + path
        case .file(let path): "file:" + path
        }
    }

    private func nextRun() throws(BroError) -> JsonValue {
        guard let run = lock.withLock({ runs.isEmpty ? nil : runs.removeFirst() }) else {
            throw BroError(MessageCode.wire(.internalUnexpected), [("detail", .string("no makemkv run left"))])
        }
        return run
    }

    private func message(_ j: JsonValue?, _ severity: Severity) -> BroMessage? {
        guard let j, let code = j["code"]?.string.flatMap(MessageCode.init(rawValue:)) else { return nil }
        return BroMessage(code, j["params"]?.members ?? [], severity: severity)
    }

    /// What a scripted run ends with: its outcome and what it reported.
    private func ran(_ run: JsonValue, _ sink: any RunSink, _ produced: [String]) -> MakemkvRun {
        for line in run["robot"]?.array ?? [] { if let e = Robot.parseLine(line.string ?? "") { sink.event(e) } }
        if run["cancel"]?.bool == true { cancel.cancel() }
        let readErrors = (run["readErrors"]?.array ?? []).map { RobotMessage(code: 2003, flags: 0, text: $0.string ?? "", format: $0.string ?? "") }
        let outcome = RunOutcome(exitCode: Int(run["exitCode"]?.int ?? 0), readErrors: readErrors,
                                 status: run["status"]?.string.flatMap(StatusWord.init(rawValue:)) ?? .success, error: message(run["error"], .error),
                                 produced: run["produced"]?.array?.map { $0.string ?? "" } ?? produced, abandoned: run["abandoned"]?.bool ?? false)
        return MakemkvRun(outcome: outcome, notice: notice(run["notice"]?.string), transcriptProblem: message(run["transcriptProblem"], .warning),
                          settingsProblem: message(run["settingsProblem"], .warning), isolationWarning: message(run["isolationWarning"], .warning))
    }

    private func notice(_ name: String?) -> MakemkvNotice? {
        switch name {
        case "keyExpired": .keyExpired
        case "evaluationNotStarted": .evaluationNotStarted
        case "versionTooOld": .versionTooOld
        case "libreDriveRequired": .libreDriveRequired
        default: nil
        }
    }

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

    func stagingDirectory(_ libraryId: String, jobId: Id) throws(BroError) -> String {
        let path = try require(libraryId).path + "/.bromelia/staging/" + jobId.value
        fs.putFolder(path)
        return path
    }
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
        call("listing " + describe(source))
        if given["makemkv"]?["runs"] != nil {
            let run = try nextRun()
            var listed = ListingBuilder()
            if let disc = run["listing"] {
                for line in Fixtures.generatedListing(disc).split(separator: "\n") {
                    if let e = Robot.parseLine(String(line)) {
                        sink.event(e)
                        listed.feed(e)
                    }
                }
            }
            return ListingRun(listing: listed.build(), run: ran(run, sink, []))
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
                          run: MakemkvRun(outcome: outcome, notice: notice, libreDrive: m["libreDrive"]?.string, version: m["version"]?.string,
                                          transcriptProblem: message(m["transcriptProblem"], .warning), settingsProblem: message(m["settingsProblem"], .warning),
                                          isolationWarning: message(m["isolationWarning"], .warning)))
    }

    func scanDrives(_ cancel: CancellationToken) async throws(BroError) -> [MakemkvDrive] { fatalError("not used") }

    /// given.matroska: a probe by file name ({duration, tracks, chapters} or null), missing, cancelled.
    func probe(_ file: String, cancel: CancellationToken) async throws(BroError) -> MkvProbe? {
        call("probe " + file)
        let m = given["matroska"] ?? .object([])
        if m["missing"]?.bool == true { throw BroError(MessageCode.wire(.toolMissing), [("tool", .string("mkvmerge"))]) }
        if m["cancelled"]?.bool == true { throw BroError(MessageCode.wire(.jobCancelled)) }
        guard let p = m[file.split(separator: "/").last.map(String.init) ?? file], p.members != nil else { return nil }
        return MkvProbe(durationSeconds: p["duration"]?.double,
                        tracks: (p["tracks"]?.array ?? []).enumerated().map { MkvTrack(id: $0.offset, type: $0.element.string ?? "") },
                        chapterCount: Int(p["chapters"]?.int ?? 0))
    }

    func remux(_ arguments: [String], cancel: CancellationToken) async throws(BroError) -> Bool { fatalError("not used") }
    func split(_ chapters: [Int], input: String, cancel: CancellationToken) async throws(BroError) -> [String]? { fatalError("not used") }
    func chapterTimes(_ file: String, cancel: CancellationToken) async throws(BroError) -> [Duration]? { fatalError("not used") }
    func rip(_ source: MakemkvSource, title: String, destination: String, invocation: MakemkvInvocation, sink: any RunSink,
             cancel: CancellationToken) async throws(BroError) -> MakemkvRun {
        call("rip \(describe(source)) \(title) \(destination)")
        let run = try nextRun()
        let files = (run["files"]?.array ?? []).map { $0.string ?? "" }
        for f in files { fs.put(destination + "/" + f, []) }
        return ran(run, sink, files)
    }

    func backup(_ source: MakemkvSource, decrypt: Bool, destination: String, invocation: MakemkvInvocation, sink: any RunSink,
                cancel: CancellationToken) async throws(BroError) -> MakemkvRun {
        call("backup \(describe(source)) \(decrypt) \(destination)")
        let run = try nextRun()
        let name = destination.split(separator: "/").last.map(String.init) ?? destination
        var produced: [String]
        if run["image"]?.bool == true {
            fs.put(destination, [UInt8](repeating: 0, count: 32769) + Array((run["header"]?.string ?? "").utf8))
            produced = [name]
        } else {
            let files = (run["files"]?.array ?? []).map { $0.string ?? "" }
            fs.putFolder(destination)
            for f in files { fs.put(destination + "/" + f, []) }
            produced = Array(Set(files.map { String($0.split(separator: "/")[0]) })).sorted { $0.utf8.lexicographicallyPrecedes($1.utf8) }
        }
        return ran(run, sink, produced)
    }

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

/// The canonical encoding on one line, as JobLog writes it.
private func oneLine(_ value: JsonValue) -> String {
    let canonical = Array(value.description.utf8)
    var out: [UInt8] = []
    var i = 0
    while i < canonical.count {
        if canonical[i] != UInt8(ascii: "\n") {
            out.append(canonical[i])
        } else {
            while i + 1 < canonical.count && canonical[i + 1] == UInt8(ascii: " ") { i += 1 }
        }
        i += 1
    }
    return String(decoding: out, as: UTF8.self)
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

/// shared/fixtures/engine/video-steps.cases.json, plan-steps.cases.json and acquire-steps.cases.json: each step alone,
/// with fake ports.
struct VideoStepsTests {
    static let steps: [StepKind: any Step] = Dictionary(uniqueKeysWithValues: ([AwaitMediaStep(), ProbeStep(), ReconcileStep(), IdentifyStep(), PlanStep(),
                                                                                DecideStep(), AcquireStep(), ReleaseStep(), VerifyRipsStep()] as [any Step]).map { ($0.kind, $0) })

    @Test func theSharedCasesPass() async throws { try await cases("engine/video-steps.cases.json") }

    @Test func thePlanCasesPass() async throws { try await cases("engine/plan-steps.cases.json") }

    @Test func theAcquireCasesPass() async throws { try await cases("engine/acquire-steps.cases.json") }

    @Test func theVerifyRipsCasesPass() async throws { try await cases("engine/verify-rips.cases.json") }

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
        outputs += (given["outputs"]?.array ?? []).map {
            StepOutput(kind: StepKind(rawValue: $0["kind"]?.string ?? "")!, data: $0["data"]!, readErrors: Int($0["readErrors"]?.int ?? 0))
        }
        let fs = fakes.fs
        let jobId = Id("00000000-0000-4000-8000-000000000001")
        let staging = (given["libraries"]?["lib"]?["path"]?.string ?? "/lib") + "/.bromelia/staging/" + jobId.value
        for t in given["tree"]?.array ?? [] {
            let path = t.string ?? ""
            if path.hasSuffix("/") { fs.putFolder(staging + "/" + path.dropLast()) } else { fs.put(staging + "/" + path, []) }
        }
        for image in given["images"]?.members ?? [] {
            fs.put(staging + "/" + image.key, [UInt8](repeating: 0, count: 32769) + Array((image.value.string ?? "").utf8))
        }
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
        let unused = UnusedTools()
        let cancel = fakes.cancel
        func contextOf(_ job: JobSnapshot) -> StepContext {
            StepContext(job: job, cancel: cancel.token, progress: fakes, log: fakes, decisions: fakes, tools: ToolSet(makemkv: fakes, matroska: fakes),
                        fs: fs, clock: ManualClock(), drives: fakes, library: fakes, catalog: fakes, config: config, driveLookup: fakes,
                        makemkvSetup: fakes, space: fakes, checkpoint: fakes, jobDirectory: "/data/jobs/" + job.id.value, messages: Renderer(),
                        appVersion: "test", finishing: finishing)
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
            if let acquisition = resources["acquisition"]?.bool { try Fixtures.same(acquisition, step.resources(job).acquisition, "resources.acquisition") }
        }
        var report: StepReport?
        var error: BroError?
        if let resumes = expect["resumes"] {
            try Fixtures.same(resumes["resumability"]?.string, step.resumability.rawValue, "resumability")
            try Fixtures.same(resumes["checkpoint"]?.string, step.resumeCheckpoint, "resumeCheckpoint")
        }
        fs.recording = true
        do throws(BroError) {
            report = try await step.run(context)
            error = report?.error
        } catch let e {
            error = e
        }
        fs.recording = false
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
        if let readErrors = expect["readErrors"]?.int { try Fixtures.same(Int(readErrors), report?.readErrors ?? 0, "readErrors") }
        if let tree = expect["tree"] {
            let files = fs.exists(staging) ? try FileTree.files(fs, root: staging) : []
            try Fixtures.same(tree, .array(files.map { .string($0) }), "tree")
        }
        if let ops = expect["fs"]?.array {
            try Fixtures.same(ops.map { $0.string ?? "" }.joined(separator: " | "), fs.ops.joined(separator: " | "), "fs")
        }
        for (name, wanted) in expect["json"]?.members ?? [] {
            var want = wanted
            if wanted.string == "probe", let disc = given["probe"]?["disc"] {
                var builder = ListingBuilder()
                for line in Fixtures.generatedListing(disc).split(separator: "\n") { if let e = Robot.parseLine(String(line)) { builder.feed(e) } }
                want = Listing.encode(builder.build())
            }
            try Fixtures.same(want, JsonValue.parse(try fs.read(staging + "/" + name)), "json " + name)
        }
        if let question = expect["question"] {
            try Fixtures.same(question["code"], fakes.question.map { .string($0.question.code.rawValue) } ?? .null, "question")
            try Fixtures.same(question["params"] ?? .object([]), JsonValue.object(fakes.question?.question.params ?? []), "question params")
        }
    }
}
