import BroDomain
import BroEngine
import BroFoundation
import BroPorts
import BroTestSupport
import Foundation
import Testing

/// Jobs and steps (MemoryStore, jobs in the order given) with the library harness's units and libraries.
private final class RecoveryStore: Store, @unchecked Sendable {
    let memory = MemoryStore()
    let db = Db()
    func jobs() -> any JobRepository { memory }
    func steps() -> any StepRepository { memory }
    func units() -> any UnitRepository { db }
    func libraries() -> any LibraryRepository { db }
    func catalog() -> any CatalogRepository { fatalError("not used") }
    func checks() -> any CheckRepository { fatalError("not used") }
    func replicas() -> any ReplicaRepository { fatalError("not used") }
    func drives() -> any DriveRepository { fatalError("not used") }
    func lookupCache() -> any LookupCacheRepository { fatalError("not used") }
    func outbox() -> any OutboxRepository { fatalError("not used") }
    func kv() -> any KeyValueRepository { fatalError("not used") }
    func transaction<T: Sendable>(_ block: @Sendable (any Store) async throws(BroError) -> T) async throws(BroError) -> T { try await block(self) }
    func migrate() async throws(BroError) {}
}

/// A step that only says what it can do after a stop; a resumeFrom one resumes from a "backup" checkpoint.
private struct RecoveryStep: Step {
    let kind: StepKind
    let resumability: Resumability
    var resumeCheckpoint: String? { resumability == .resumeFrom ? "backup" : nil }
    func resources(_ job: JobSnapshot) -> ResourceSet { .none }
    func run(_ context: StepContext) async throws(BroError) -> StepReport { fatalError("not used") }
}

private final class Isolation: SettingsIsolation, @unchecked Sendable {
    private let lock = NSLock()
    private var entries: [String] = []
    var problems: [BroMessage] = []
    var log: [String] { lock.withLock { entries } }

    func prepare(_ settings: MakemkvRunSettings) throws(BroError) -> any IsolationLease { fatalError("not used") }

    func recover(_ workDirectories: [String]) -> [BroMessage] {
        lock.withLock { entries.append((["recover"] + workDirectories).joined(separator: " ")) }
        return problems
    }
}

/// '<code>' then ' <key>=<value>' per param (strings as they are, other values as JSON).
private struct Renderer: MessageRenderer {
    func render(_ message: BroMessage) -> String {
        message.code.rawValue + message.params.map { p in
            " " + p.key + "=" + (p.value.string ?? String(decoding: JsonValue.encodeCanonical(p.value), as: UTF8.self))
        }.joined()
    }
}

@EngineActor
private final class RecoveryWorld {
    var events: [EngineEvent] = []
    var results: [Int: JsonValue] = [:]
    var recovered = false
    var subscription: Subscription?
    var service: RecoveryService!
    var problems: ProblemService!
}

private func caseError(_ j: JsonValue) -> BroError { BroError(j["code"]!.string!, j["params"]?.members ?? []) }

private func errorJson(_ e: BroError?) -> JsonValue? {
    guard let e else { return nil }
    var m: [(key: String, value: JsonValue)] = [("code", .string(e.code))]
    if !e.params.isEmpty { m.append(("params", .object(e.params))) }
    return .object(m)
}

private func projection(_ r: JobRecord) -> JsonValue {
    var m: [(key: String, value: JsonValue)] = [
        ("state", .string(r.state.rawValue)), ("queue", .string(r.queue.rawValue)),
        ("plan", r.plan.map { .array($0.steps.map { .string($0.rawValue) }) } ?? .null),
        ("mediaGeneration", r.mediaGeneration.map { .integer($0) } ?? .null),
    ]
    if let o = r.outcome { m.append(("outcome", .string(o.rawValue))) }
    if let e = errorJson(r.error) { m.append(("error", e)) }
    if let d = r.decision { m.append(("decision", d)) }
    return .object(m)
}

/// shared/fixtures/engine/recovery-service.cases.json, on a real executor, event bus, problem service, scheduler and library
/// service, with the library harness's file tree and database, and jobs and steps in memory.
struct RecoveryServiceTests {
    @Test func theSharedCasesPass() async throws {
        let doc = try Fixtures.json("engine/recovery-service.cases.json")
        var failures: [String] = []
        for c in doc["cases"]!.array! {
            let id = c["id"]!.string!
            let world = World()
            do {
                try await Self.run(c["given"]!, c["expect"]!, world)
            } catch {
                failures.append("\(id): \(error)")
            }
            await world.executor.stop()
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    private static func run(_ given: JsonValue, _ expect: JsonValue, _ world: World) async throws {
        let tree = Tree()
        for d in given["dirs"]?.array ?? [] { tree.makeDirs(d.string!) }
        for (p, t) in given["files"]?.members ?? [] { tree.put(p, t.string!) }
        for h in given["hold"]?.array ?? [] { tree.holds.append((h["op"]!.string!, h["path"]!.string!)) }
        let store = RecoveryStore()
        let db = store.db
        for l in given["libraries"]?.array ?? [] {
            db.libraries[l["id"]!.string!] = LibraryRecord(id: l["id"]!.string!, name: l["name"]!.string!, path: l["path"]!.string!,
                                                           markerId: l["markerId"]!.string!, attachedAt: Instant.parse(l["attachedAt"]!.string!)!,
                                                           online: l["online"]!.bool!, lastSeenAt: l["lastSeenAt"]?.string.flatMap(Instant.parse))
        }
        for u in given["units"]?.array ?? [] {
            db.units[u["id"]!.string!] = UnitRecord(id: Id(u["id"]!.string!), libraryId: u["library"]!.string!, jobId: Id(u["job"]!.string!),
                                                    path: u["path"]!.string!, recordFile: "bromelia-00000000.json", recordVersion: 3, state: .committing,
                                                    status: .success, name: u["name"]!.string!, kind: .movie, format: "bluray", formatCode: "BD",
                                                    encrypted: false, label: "", makemkvVersion: "", bytes: 0, fileCount: 0, attempts: 1,
                                                    createdAt: world.clock.now())
        }
        for i in given["intents"]?.array ?? [] {
            let items = i["items"]!.array!.map { x in
                CommitItem(seq: Int(x.array![0].int!), fromPath: x.array![1].string!, toPath: x.array![2].string!, sha256: x.array![3].string,
                           isDir: x.array![4].bool!, moved: x.array![5].bool!)
            }
            db.intents[Id(i["unit"]!.string!)] = CommitIntent(unitId: Id(i["unit"]!.string!), jobId: Id(i["job"]!.string!), staging: i["staging"]!.string!,
                                                              destination: i["destination"]!.string!, merge: i["merge"]!.bool!, quarantine: false,
                                                              createdAt: world.clock.now(), items: items)
        }
        for j in given["jobs"]?.array ?? [] {
            let jobId = Id(j["id"]!.string!)
            let generation = j["mediaGeneration"]?.int
            let request = JobRequest(kind: .videoDisc, title: j["title"]!.string!, automatic: j["automatic"]?.bool == true, driveId: j["driveId"]?.string,
                                     mediaGeneration: generation)
            let plan = j["plan"]?.array.map { JobPlan(steps: $0.map { StepKind(rawValue: $0.string!)! }) }
            try await store.memory.insert(JobRecord(
                id: jobId, kind: .videoDisc, parentId: nil, state: JobState(rawValue: j["state"]!.string!)!,
                outcome: j["outcome"]?.string.flatMap(Outcome.init(rawValue:)), error: j["error"].map(caseError), blockedBy: nil, decision: j["decision"],
                queue: Queue(rawValue: j["queue"]?.string ?? "acquisition")!, position: 0, driveId: request.driveId, mediaGeneration: generation,
                automatic: request.automatic, mode: nil, title: request.title, fingerprint: nil, request: request, plan: plan,
                unitId: j["unitId"]?.string.map(Id.init), createdAt: world.clock.now()))
            for (seq, r) in j["rows"]!.array!.enumerated() {
                let kind = StepKind(rawValue: r["kind"]!.string!)!
                try await store.memory.save(StepRecord(jobId: jobId, seq: seq, kind: kind, state: StepState(rawValue: r["state"]!.string!)!, attempt: 1,
                                                       startedAt: world.clock.now(), finishedAt: nil, output: StepOutput(kind: kind, data: .object([]),
                                                                                                                    readErrors: Int(r["readErrors"]?.int ?? 0)),
                                                       error: r["error"].map(caseError), checkpoint: r["checkpoint"]))
            }
        }
        let resumability = given["resumability"]?.members ?? []
        let registry = StepRegistry(StepKind.allCases.map { k in
            RecoveryStep(kind: k, resumability: resumability.first { $0.key == k.rawValue }.flatMap { Resumability(rawValue: $0.value.string!) } ?? .idempotent)
        })
        let isolation = Isolation()
        isolation.problems = (given["isolationProblems"]?.array ?? []).map(BroMessage.decode)
        let config = await CaseConfig()
        let libraries = given["config"] ?? .array([])
        await world.onExecutor { config.document = .object([("version", .integer(3)), ("libraries", libraries)]) }

        let state = await RecoveryWorld()
        await world.onExecutor {
            let bus = EventBus(clock: world.clock)
            let problems = ProblemService(bus: bus, clock: world.clock)
            state.problems = problems
            let scheduler = Scheduler(executor: world.executor, bus: bus, limits: SchedulerLimits())
            let library = LibraryService(executor: world.executor, bus: bus, problems: problems, scheduler: scheduler, config: config,
                                         clock: world.engineClock, fs: tree, libraries: db, units: db)
            state.service = RecoveryService(executor: world.executor, bus: bus, problems: problems, store: store, registry: registry, library: library,
                                      config: config, clock: world.engineClock, fs: tree, isolation: isolation, renderer: Renderer(), dataDirectory: "/data")
            state.subscription = bus.subscribe(types: [.jobChanged, .jobFinished, .problemRaised]) { state.events.append($0) }
        }

        @Sendable func settled() async -> Bool {
            await withCheckedContinuation { (done: CheckedContinuation<Bool, Never>) in
                // Posted alone (no barrier after it), so the executor's activity counts only the engine's own work.
                if !world.executor.post({ done.resume(returning: world.executor.activity == tree.held) }) { done.resume(returning: true) }
            }
        }
        @Sendable func settle() async throws {
            let until = Date().addingTimeInterval(10)
            while !(await settled()) {
                if Date() > until { throw FixtureError("the service didn't settle") }
                try await Task.sleep(nanoseconds: 1_000_000)
            }
        }

        let ops = given["ops"]!.array!
        var pending: [Task<Void, Never>] = []
        try await settle()
        for (i, op) in ops.enumerated() {
            let (name, arg): (String, JsonValue) = op.string.map { ($0, .object([])) } ?? (op.members![0].key, op.members![0].value)
            switch name {
            case "recover":
                let task = Task { @EngineActor in
                    do throws(BroError) {
                        state.results[i] = try await state.service.recover()?.toJson() ?? .null
                    } catch {
                        state.results[i] = .object([("error", errorJson(error)!)])
                    }
                    state.recovered = true
                }
                if arg["wait"]?.bool == false { pending.append(task) } else { await task.value }
                try await settle()
            case "recovered":
                try await settle()
                await world.onExecutor { state.results[i] = .bool(state.recovered) }
            case "advance":
                await world.clock.advance(Int64((arg.double! * 1000).rounded())) { try? await settle() }
                try await settle()
                await world.onExecutor { state.results[i] = .string("ok") }
            case "release":
                tree.release()
                let until = Date().addingTimeInterval(10)
                while tree.held > 0 {
                    if Date() > until { throw FixtureError("the held calls didn't leave") }
                    try await Task.sleep(nanoseconds: 1_000_000)
                }
                try await settle()
                await world.onExecutor { state.results[i] = .string("ok") }
            default:
                throw FixtureError("unknown op \(name)")
            }
        }
        for t in pending { await t.value }
        try await settle()

        let results = await world.onExecutorValue { (0..<ops.count).map { state.results[$0] ?? .null } }
        try Fixtures.same(expect["results"]!, .array(results), "results")
        for (jobId, want) in expect["jobs"]?.members ?? [] {
            try Fixtures.same(want, projection(store.memory.job(Id(jobId))!), "job \(jobId)")
        }
        for (jobId, want) in expect["steps"]?.members ?? [] {
            try Fixtures.same(want, .array(store.memory.stepsOf(Id(jobId)).map {
                .string("\($0.kind.rawValue) \($0.state.rawValue)\($0.checkpoint != nil ? " checkpoint" : "")")
            }), "steps of \(jobId)")
        }
        for (jobId, want) in expect["logs"]?.members ?? [] {
            var lines: [JsonValue] = []
            if let bytes = tree.files["/data/jobs/\(jobId)/log.jsonl"] {
                for l in String(decoding: bytes, as: UTF8.self).split(separator: "\n") {
                    if l.contains("\"seq\"") { throw FixtureError("a log line has seq: \(l)") }
                    let o = JsonValue.parse(Array(l.utf8))!
                    var m: [(key: String, value: JsonValue)] = [("severity", o["severity"]!), ("code", o["code"]!)]
                    if let p = o["params"], !(p.members ?? []).isEmpty { m.append(("params", p)) }
                    lines.append(.object(m))
                }
            }
            try Fixtures.same(want, .array(lines), "log of \(jobId)")
        }
        let (lines, problemsJson) = await world.onExecutorValue { () -> ([JsonValue], [JsonValue]) in
            let lines: [JsonValue] = state.events.map { e in
                let job = e.data["job"]
                switch e.type {
                case .jobChanged: return .string("jobChanged \(job!["id"]!.string!) \(job!["state"]!.string!)")
                case .jobFinished:
                    return .string("jobFinished \(job!["id"]!.string!) \(job!["outcome"]!.string!)" + (job!["error"]?["code"]?.string.map { " " + $0 } ?? ""))
                default: return .string("problemRaised \(e.data["problem"]!["code"]!.string!)")
                }
            }
            return (lines, state.problems.problems().map(ProblemDto.json))
        }
        if let el = expect["eventLines"] { try Fixtures.same(el, .array(lines), "event lines") }
        if let pr = expect["problems"] { try Fixtures.same(pr, .array(problemsJson), "problems") }
        if let iso = expect["isolation"] { try Fixtures.same(iso, .array(isolation.log.map { .string($0) }), "isolation") }
        if let fl = expect["files"] {
            let got = tree.files.filter { !($0.key.hasPrefix("/data/jobs/") && $0.key.hasSuffix("/" + JobLog.fileName)) }
                .sorted { ordinal($0.key, $1.key) }.map { (key: $0.key, value: JsonValue.string(String(decoding: $0.value, as: UTF8.self))) }
            let want = (fl.members ?? []).sorted { ordinal($0.key, $1.key) }
            try Fixtures.same(JsonValue.object(want), .object(got), "files")
        }
        for (folder, want) in expect["dirsUnder"]?.members ?? [] {
            try Fixtures.same(want, .array(tree.dirs.filter { $0.hasPrefix(folder + "/") }.sorted(by: ordinal).map { .string($0) }), "folders under \(folder)")
        }
        if let wantDb = expect["db"] {
            if let units = wantDb["units"] {
                try Fixtures.same(units, .array(db.units.values.sorted { ordinal($0.id.value, $1.id.value) }.map {
                    .object([("id", .string($0.id.value)), ("state", .string($0.state.rawValue))])
                }), "db units")
            }
            if let intents = wantDb["intents"] { try Fixtures.same(intents, .array(db.open().map { .string($0.unitId.value) }), "db intents") }
        }
    }
}

private extension World {
    /// Runs `body` on the executor and returns what it gives.
    func onExecutorValue<T: Sendable>(_ body: @escaping @EngineActor @Sendable () -> T) async -> T {
        await withCheckedContinuation { (done: CheckedContinuation<T, Never>) in
            executor.post { done.resume(returning: body()) }
        }
    }
}
