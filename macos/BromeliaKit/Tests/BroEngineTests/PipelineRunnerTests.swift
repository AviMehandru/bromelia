import BroDomain
import BroEngine
import BroFoundation
import BroPorts
import BroTestSupport
import Foundation
import Testing

/// Gates a fake step can wait at, until the test opens them or the job is cancelled. Thread-safe.
final class Gates: @unchecked Sendable {
    private let lock = NSLock()
    private var opened: Set<String> = []
    private var waiters: [(gate: String, job: Id, done: CheckedContinuation<Bool, Never>, id: Int)] = []
    private var cancelledJobs: Set<Id> = []
    private var next = 0

    /// True when the gate opened, false when the job was cancelled first.
    func wait(_ gate: String, job: Id, cancel: CancellationToken) async -> Bool {
        if lock.withLock({ opened.contains(gate) }) { return true }
        var unregister: (@Sendable () -> Void)?
        let result = await withCheckedContinuation { (done: CheckedContinuation<Bool, Never>) in
            let id = lock.withLock { () -> Int? in
                if opened.contains(gate) { return nil }
                next += 1
                waiters.append((gate, job, done, next))
                return next
            }
            guard let id else {
                done.resume(returning: true)
                return
            }
            unregister = cancel.onCancel { [self] in
                let w = lock.withLock { () -> CheckedContinuation<Bool, Never>? in
                    guard let i = waiters.firstIndex(where: { $0.id == id }) else { return nil }
                    return waiters.remove(at: i).done
                }
                w?.resume(returning: false)
            }
        }
        unregister?()
        return result
    }

    func open(_ gate: String) {
        let ready = lock.withLock { () -> [CheckedContinuation<Bool, Never>] in
            opened.insert(gate)
            let r = waiters.filter { $0.gate == gate }.map(\.done)
            waiters.removeAll { $0.gate == gate }
            return r
        }
        ready.forEach { $0.resume(returning: true) }
    }

    func cancelled(_ job: Id) { lock.withLock { _ = cancelledJobs.insert(job) } }

    /// The jobs waiting at a closed gate (not cancelled), one entry per waiting step.
    func parked() -> [Id] { lock.withLock { waiters.map(\.job).filter { !cancelledJobs.contains($0) } } }

    func isCancelled(_ job: Id) -> Bool { lock.withLock { cancelledJobs.contains(job) } }
}

/// A step that does what the case says.
struct FakeStep: Step {
    let kind: StepKind
    let behaviour: JsonValue
    let gates: Gates
    var resumability: Resumability { .idempotent }

    static func strings(_ j: JsonValue?) -> [String] { (j?.array ?? []).map { $0.string! } }

    static func message(_ j: JsonValue, severity: Severity = .info) -> BroMessage {
        BroMessage(MessageCode(rawValue: j["code"]!.string!)!, j["params"]?.members ?? [], severity: severity)
    }

    func resources(_ job: JobSnapshot) -> ResourceSet {
        let r = behaviour["resources"]
        var drives = FakeStep.strings(r?["drives"])
        if behaviour["drive"]?.bool == true, let d = job.request.driveId { drives.append(d) }
        return ResourceSet(drives: drives, acquisition: r?["acquisition"]?.bool == true, libraryWrites: FakeStep.strings(r?["libraryWrites"]),
                           cpu: Int(r?["cpu"]?.int ?? 0), io: FakeStep.strings(r?["io"]))
    }

    func run(_ context: StepContext) async throws(BroError) -> StepReport {
        for p in behaviour["progress"]?.array ?? [] {
            context.progress.report(p.array![0].double!, operation: BroMessage(MessageCode(rawValue: p.array![1].string!)!), current: nil)
        }
        for code in FakeStep.strings(behaviour["log"]) { context.log.log(BroMessage(MessageCode(rawValue: code)!)) }
        var data = behaviour["output"] ?? .object([])
        if let key = behaviour["config"]?.string { data = .object([(key, context.config.document[key] ?? .null)]) }
        if let echo = behaviour["echo"]?.string { data = context.job.outputs.last { $0.kind.rawValue == echo }!.data }
        if let ask = behaviour["ask"] {
            let question = DecisionDto(id: ask["id"]!.string!, question: FakeStep.message(ask["question"]!),
                                       answers: FakeStep.strings(ask["answers"]).map {
                                           DecisionAnswer(id: $0, label: BroMessage(MessageCode(rawValue: "decision.answer." + $0)!))
                                       })
            let answer = try await context.decisions.ask(question)
            data = .object([("answer", .string(answer))])
        }
        if let gate = behaviour["gate"]?.string, !(await gates.wait(gate, job: context.job.id, cancel: context.cancel)) {
            throw BroError(MessageCode.wire(.jobCancelled))
        }
        let notice: MakemkvNotice? = switch behaviour["notice"]?.string {
        case "keyExpired": .keyExpired
        case "evaluationNotStarted": .evaluationNotStarted
        case "versionTooOld": .versionTooOld
        case "libreDriveRequired": .libreDriveRequired
        default: nil
        }
        return StepReport(data: data, appendSteps: FakeStep.strings(behaviour["append"]).map { StepKind(rawValue: $0)! },
                          skipped: behaviour["skipped"]?.bool == true, readErrors: Int(behaviour["readErrors"]?.int ?? 0),
                          quarantined: behaviour["quarantined"]?.bool == true, notice: notice,
                          skipJob: behaviour["skipJob"].map { FakeStep.message($0, severity: .warning) },
                          error: behaviour["fail"].map { BroError($0["code"]!.string!, $0["params"]?.members ?? []) },
                          affectsOutcome: behaviour["affectsOutcome"]?.bool ?? true)
    }
}

@EngineActor
final class PipelineHarness {
    let world = World()
    let gates: Gates
    let bus: EventBus
    let scheduler: Scheduler
    let store = MemoryStore()
    let fs = MemoryFileSystem()
    let config = CaseConfig()
    let runner: PipelineRunner
    var jobs: [(name: String, id: Id)] = []
    var outcomes: [String: Decided] = [:]
    var events: [EngineEvent] = []
    var started: Set<String> = []
    var subscription: Subscription?

    init(_ given: JsonValue) async throws {
        let gates = Gates()
        self.gates = gates
        bus = EventBus(clock: world.clock)
        scheduler = Scheduler(executor: world.executor, bus: bus, limits: SchedulerLimits())
        let steps: [any Step] = StepKind.allCases.map { FakeStep(kind: $0, behaviour: given["steps"]?[$0.rawValue] ?? .object([]), gates: gates) }
        let unused = UnusedTools()
        if let document = given["config"] { config.document = document }
        let env = StepEnvironment(tools: ToolSet(makemkv: unused, matroska: unused), fs: fs, clock: world.clock, drives: unused, library: unused,
                                  catalog: unused, config: config, driveLookup: unused, makemkvSetup: unused, dataDirectory: "/data")
        runner = PipelineRunner(executor: world.executor, bus: bus, scheduler: scheduler, store: store, registry: StepRegistry(steps),
                                environment: env, clock: world.engineClock)
        subscription = bus.subscribe(types: []) { [unowned self] e in self.events.append(e) }
        var n = 0
        for j in given["jobs"]!.array! {
            n += 1
            let hex = String(n, radix: 16)
            let id = Id("00000000-0000-4000-8000-" + String(repeating: "0", count: 12 - hex.count) + hex)
            let name = j["name"]!.string!
            jobs.append((name, id))
            let kind = JobKind(rawValue: j["kind"]?.string ?? "videoDisc")!
            let title = j["title"]?.string ?? name
            let request = JobRequest(kind: kind, title: title, automatic: j["automatic"]?.bool == true, driveId: j["driveId"]?.string)
            let plan = j["plan"].map { JobPlan(steps: FakeStep.strings($0).map { StepKind(rawValue: $0)! }) }
            let record = JobRecord(id: id, kind: kind, state: .queued, queue: Queue(rawValue: j["queue"]?.string ?? "acquisition")!, position: 0,
                                   driveId: request.driveId, automatic: request.automatic, title: title, request: request, plan: plan,
                                   createdAt: world.clock.now())
            try await store.insert(record)
            var seq = 0
            for d in j["done"]?.array ?? [] {
                let k = StepKind(rawValue: d["kind"]!.string!)!
                try await store.save(StepRecord(jobId: id, seq: seq, kind: k, state: StepState(rawValue: d["state"]?.string ?? "succeeded")!, attempt: 1,
                                                startedAt: world.clock.now(), finishedAt: world.clock.now(),
                                                output: StepOutput(kind: k, data: d["output"] ?? .object([]))))
                seq += 1
            }
            if let log = j["log"] {
                let lines = FakeStep.strings(log).enumerated().map { n, c in
                    LogLineDto(seq: Int64(n + 1), t: world.clock.now(), severity: .info, code: MessageCode(rawValue: c)!, params: [])
                }
                try fs.createDirectory("/data/jobs/\(id.value)", parentsMustExist: false)
                try fs.writeAtomically("/data/jobs/\(id.value)/log.jsonl", bytes: JobLog.encode(lines), mode: 0o600)
            }
        }
    }

    func id(_ name: String) -> Id { jobs.first { $0.name == name }!.id }
    func name(_ id: String) -> String { jobs.first { $0.id.value == id }!.name }

    /// The engine has settled: everything still going on is parked (a step at a closed gate, a step waiting for an
    /// answer), and every job that was run has finished or waits (for resources, an answer, a gate).
    func check() -> Bool {
        let atGate = gates.parked()
        var parked = atGate.count
        var ok = true
        for (name, id) in jobs where started.contains(name) && outcomes[name] == nil {
            let last = events.last { ($0.type == .jobChanged || $0.type == .jobFinished) && $0.data["job"]?["id"]?.string == id.value }
            let state = last?.data["job"]?["state"]?.string
            if state == "awaitingDecision" && !gates.isCancelled(id) {
                parked += 1
            } else if !(state == "waitingForResources" || state == "blocked" || atGate.contains(id)) {
                ok = false
            }
        }
        return ok && world.executor.activity == parked
    }

    nonisolated func settled() async -> Bool {
        await withCheckedContinuation { (done: CheckedContinuation<Bool, Never>) in
            // Posted alone (no barrier after it), so the executor's activity counts only the engine's own work.
            if !world.executor.post({ [self] in done.resume(returning: self.check()) }) { done.resume(returning: true) }
        }
    }

    nonisolated func settle() async throws {
        let until = Date().addingTimeInterval(10)
        while !(await settled()) {
            if Date() > until { throw FixtureError("the jobs didn't settle") }
            try await Task.sleep(nanoseconds: 1_000_000)
        }
    }

    func run(_ op: JsonValue) async throws {
        let (name, arg) = op.members!.first!
        switch name {
        case "run":
            let job = arg.string!, jobId = id(job)
            started.insert(job)
            Task { @EngineActor in
                if let d = try? await self.runner.run(jobId) { self.outcomes[job] = d }
            }
        case "open":
            gates.open(arg.string!)
        case "cancel":
            gates.cancelled(id(arg.string!))
            runner.cancel(id(arg.string!))
        case "decide":
            var code: String?
            do throws(BroError) {
                try runner.decide(id(arg["job"]!.string!), answer: arg["answer"]!.string!)
            } catch {
                code = error.code
            }
            try Fixtures.same(arg["expectError"]?.string, code, "decide's error")
        case "advance":
            await world.advance(arg.int!)
        case "config":
            config.document = arg
        case "library":
            let lib = arg["id"]!.string!
            scheduler.setLibraryOffline(lib, reason: arg["online"]!.bool! ? nil : BroError(MessageCode.wire(.libraryOffline), [("library", .string(lib))]))
        default:
            throw FixtureError("unknown op \(name)")
        }
    }

    func lines() -> [String: [String]] {
        var lines: [String: [String]] = [:]
        for (n, _) in jobs { lines[n] = [] }
        for e in events {
            switch e.type {
            case .jobChanged:
                let job = e.data["job"]!
                var text = "changed \(job["state"]!.string!) \(job["progress"]?["step"]?.string ?? "-")"
                if let f = job["progress"]?["currentFraction"]?.double { text += " progress \(Int((f * 100).rounded()))" }
                if let b = job["blockedBy"]?["code"]?.string { text += " blockedBy " + b }
                if let d = job["decision"]?["id"]?.string { text += " decision " + d }
                lines[name(job["id"]!.string!)]!.append(text)
            case .stepFinished:
                lines[name(e.data["job"]!.string!)]!.append("step \(e.data["kind"]!.string!) \(e.data["state"]!.string!)")
            case .jobLog:
                for l in e.data["lines"]!.array! { lines[name(e.data["jobId"]!.string!)]!.append("log " + l["code"]!.string!) }
            case .jobFinished:
                let job = e.data["job"]!
                var text = "finished " + job["outcome"]!.string!
                if let code = job["error"]?["code"]?.string { text += " " + code }
                lines[name(job["id"]!.string!)]!.append(text)
            default:
                break
            }
        }
        return lines
    }

    func queueLines() -> [String] {
        events.filter { $0.type == .queueChanged }.map { e in
            e.data["queue"]!.string! + ": " + e.data["order"]!.array!.map { name($0.string!) }.joined(separator: " ")
        }
    }
}

/// shared/fixtures/engine/pipeline-runner.cases.json and job-planner.cases.json.
struct PipelineRunnerTests {
    static func check(_ id: String, _ given: JsonValue, _ expect: JsonValue) async throws {
        let h = try await PipelineHarness(given)
        for op in given["ops"]!.array! {
            try await h.run(op)
            try await h.settle()
        }
        let (outcomes, lines, queueLines, events) = await (h.outcomes, h.lines(), h.queueLines(), h.events.map(EngineEvent.json))
        for (name, want) in expect["outcomes"]?.members ?? [] {
            let d = outcomes[name]
            try Fixtures.same(want["outcome"]!.string, d?.outcome.rawValue, "outcome of \(name)")
            if let e = want["error"] {
                try Fixtures.same(e["code"]!.string, d?.error?.code, "error of \(name)")
                if let p = e["params"] { try Fixtures.same(p, JsonValue.object(d?.error?.params ?? []), "error params of \(name)") }
            } else {
                try Fixtures.same(nil, d?.error?.code, "error of \(name)")
            }
        }
        for (name, want) in expect["lines"]?.members ?? [] { try Fixtures.same(FakeStep.strings(want), lines[name] ?? [], "lines of \(name)") }
        if let q = expect["queueLines"] { try Fixtures.same(FakeStep.strings(q), queueLines, "queue lines") }
        for (name, want) in expect["steps"]?.members ?? [] {
            let steps = h.store.stepsOf(await h.id(name)).map { "\($0.kind.rawValue) \($0.state.rawValue)" }
            try Fixtures.same(FakeStep.strings(want), steps, "steps of \(name)")
        }
        for (name, want) in expect["records"]?.members ?? [] {
            let r = h.store.job(await h.id(name))!
            try Fixtures.same(want["state"]!.string, r.state.rawValue, "record state of \(name)")
            try Fixtures.same(want["outcome"]!.string, r.outcome?.rawValue, "record outcome of \(name)")
            try Fixtures.same(FakeStep.strings(want["plan"]), r.plan!.steps.map(\.rawValue), "record plan of \(name)")
        }
        for (name, want) in expect["logFile"]?.members ?? [] {
            let text = String(decoding: try h.fs.read("/data/jobs/\(await h.id(name).value)/log.jsonl"), as: UTF8.self)
            var codes: [String] = []
            for line in text.split(separator: "\n") {
                try Fixtures.check(!line.contains("seq"), "a log line has no seq")
                codes.append(JsonValue.parse(String(line))!["code"]!.string!)
            }
            try Fixtures.same(FakeStep.strings(want), codes, "log file of \(name)")
        }
        for (name, want) in expect["outputs"]?.members ?? [] {
            let steps = h.store.stepsOf(await h.id(name))
            for (kind, data) in want.members! {
                try Fixtures.same(data, steps.last { $0.kind.rawValue == kind }!.output!.data, "output of \(name)'s \(kind)")
            }
        }
        if let want = expect["events"] { try Fixtures.same(want, JsonValue.array(events), "events") }
        await h.world.executor.stop()
    }

    @Test func theSharedCasesPass() async throws {
        let doc = try Fixtures.json("engine/pipeline-runner.cases.json")
        var failures: [String] = []
        for c in doc["cases"]!.array! {
            let id = c["id"]!.string!
            do { try await PipelineRunnerTests.check(id, c["given"]!, c["expect"]!) } catch { failures.append("\(id): \(error)") }
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    @Test func theSharedPlannerCasesPass() throws {
        let failures = try Fixtures.runCases("engine/job-planner.cases.json") { _, given, expect in
            let r = given["request"]!
            let request = JobRequest(kind: JobKind(rawValue: r["kind"]!.string!)!, title: r["title"]!.string!, automatic: r["automatic"]?.bool == true,
                                     driveId: r["driveId"]?.string, sessionId: r["sessionId"]?.string.map { Id($0) })
            try Fixtures.same(FakeStep.strings(expect["steps"]), JobPlanner.initial(request).map(\.rawValue), "steps")
            return true
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }
}
