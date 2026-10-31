import BroDomain
import BroEngine
import BroFoundation
import BroPorts
import BroTestKit
import BroTestSupport
import Foundation
import Testing

/// Gates a fake step can wait at, until the test opens them or the job is cancelled. Thread-safe.
final class Gates: @unchecked Sendable {
    private let lock = NSLock()
    private var opened: Set<String> = []
    private var waiters: [(gate: String, job: Id, done: CheckedContinuation<Bool, Never>, id: Int, stubborn: Bool)] = []
    private var cancelledJobs: Set<Id> = []
    private var next = 0

    /// True when the gate opened, false when the job was cancelled first (never when `ignoresCancel`).
    func wait(_ gate: String, job: Id, cancel: CancellationToken, ignoresCancel: Bool = false) async -> Bool {
        if lock.withLock({ opened.contains(gate) }) { return true }
        var unregister: (@Sendable () -> Void)?
        let result = await withCheckedContinuation { (done: CheckedContinuation<Bool, Never>) in
            let id = lock.withLock { () -> Int? in
                if opened.contains(gate) { return nil }
                next += 1
                waiters.append((gate, job, done, next, ignoresCancel))
                return next
            }
            guard let id else {
                done.resume(returning: true)
                return
            }
            if ignoresCancel { return }
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

    /// The jobs waiting at a closed gate (not cancelled, or at a gate that ignores the cancel), one entry per waiting step.
    func parked() -> [Id] { lock.withLock { waiters.filter { $0.stubborn || !cancelledJobs.contains($0.job) }.map(\.job) } }

    func isCancelled(_ job: Id) -> Bool { lock.withLock { cancelledJobs.contains(job) } }
}

/// What each fake step does; the steps op changes it. Thread-safe.
final class Behaviours: @unchecked Sendable {
    private let lock = NSLock()
    private var map: [StepKind: JsonValue] = [:]
    private var ends: [Id: String] = [:]

    subscript(kind: StepKind) -> JsonValue {
        get { lock.withLock { map[kind] ?? .object([]) } }
        set { lock.withLock { map[kind] = newValue } }
    }

    /// What the Release at the end saw, by job: '<outcome> <error code>'.
    func finishing(_ job: Id) -> String? { lock.withLock { ends[job] } }

    func saw(_ job: Id, _ end: String) { lock.withLock { ends[job] = end } }
}

/// A step that does what the case says.
struct FakeStep: Step {
    let kind: StepKind
    let behaviours: Behaviours
    let gates: Gates
    var behaviour: JsonValue { behaviours[kind] }
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
        if let end = context.finishing {
            for code in FakeStep.strings(behaviour["log"]) { context.log.log(BroMessage(MessageCode(rawValue: code)!)) }
            behaviours.saw(context.job.id, end.outcome.rawValue + " " + (end.error?.code ?? ""))
            return StepReport()
        }
        for p in behaviour["progress"]?.array ?? [] {
            context.progress.report(p.array![0].double!, operation: BroMessage(MessageCode(rawValue: p.array![1].string!)!), current: nil)
        }
        for code in FakeStep.strings(behaviour["log"]) { context.log.log(BroMessage(MessageCode(rawValue: code)!)) }
        var data = behaviour["output"] ?? .object([])
        if let key = behaviour["config"]?.string { data = .object([(key, context.config.document[key] ?? .null)]) }
        if let echo = behaviour["echo"]?.string { data = context.job.outputs.last { $0.kind.rawValue == echo }!.data }
        if behaviour["context"]?.bool == true {
            data = .object([("startedAt", context.job.startedAt.map { .string(Instant.format($0)) } ?? .null), ("appVersion", .string(context.appVersion)),
                            ("message", .string(context.messages.render(BroMessage(.jobCancelled))))])
        }
        if let ask = behaviour["ask"] {
            let question = DecisionDto(id: ask["id"]!.string!, question: FakeStep.message(ask["question"]!),
                                       answers: FakeStep.strings(ask["answers"]).map {
                                           DecisionAnswer(id: $0, label: BroMessage(MessageCode(rawValue: "decision.answer." + $0)!))
                                       })
            let answer = try await context.decisions.ask(question)
            data = .object([("answer", .string(answer))])
        }
        if let checkpoint = behaviour["checkpoint"] { try await context.checkpoint.save(checkpoint) }
        if behaviour["saved"]?.bool == true { data = .object([("saved", context.checkpoint.saved ?? .null)]) }
        if let r = behaviour["reserve"] {
            try await context.space.reserve(r["volume"]!.string!, path: r["path"]!.string!, need: r["need"]!.int!, free: r["free"]!.int!)
        }
        if let gate = behaviour["gate"]?.string,
           !(await gates.wait(gate, job: context.job.id, cancel: context.cancel, ignoresCancel: behaviour["ignoresCancel"]?.bool == true)) {
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
                          affectsOutcome: behaviour["affectsOutcome"]?.bool ?? true,
                          plan: behaviour["plan"].map { JobPlan(steps: [], mode: $0["mode"]?.string, profileId: $0["profileId"]?.string, libraryId: $0["libraryId"]?.string) })
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
    let behaviours = Behaviours()
    let service: Bool
    let ids: CountingIds
    var jobService: JobService!
    var calls: [(op: JsonValue, task: Task<Void, Never>)] = []
    var callErrors: [Int: String] = [:]
    /// The ops that have ended (an op that never does fails its case instead of hanging the run).
    var callsEnded: Set<Int> = []
    var queries: [JsonValue] = []
    var logs: [JsonValue] = []
    var details: [JsonValue] = []
    var announcedUnstored: [String] = []
    /// After a stop op: the jobs' last states no longer say whether they settled.
    var stopped = false

    /// `service`: job-service cases (cancel and decide go through the JobService).
    init(_ given: JsonValue, service: Bool = false) async throws {
        self.service = service
        let gates = Gates()
        self.gates = gates
        bus = EventBus(clock: world.clock)
        scheduler = Scheduler(executor: world.executor, bus: bus, limits: SchedulerLimits(acquisitions: Int(given["limits"]?["acquisitions"]?.int ?? 0)))
        for kind in StepKind.allCases { behaviours[kind] = given["steps"]?[kind.rawValue] ?? .object([]) }
        let behaviours = behaviours
        let steps: [any Step] = StepKind.allCases.map { FakeStep(kind: $0, behaviours: behaviours, gates: gates) }
        ids = CountingIds(Int64(given["jobs"]!.array!.count))
        let unused = UnusedTools()
        if let document = given["config"] { config.document = document }
        let env = StepEnvironment(tools: ToolSet(makemkv: unused, matroska: unused), fs: fs, clock: world.clock, drives: unused, library: unused,
                                  catalog: unused, config: config, driveLookup: unused, makemkvSetup: unused, dataDirectory: "/data",
                                  messages: Renderer(), appVersion: "test")
        runner = PipelineRunner(executor: world.executor, bus: bus, scheduler: scheduler, store: store, registry: StepRegistry(steps),
                                environment: env, clock: world.engineClock)
        jobService = JobService(executor: world.executor, bus: bus, store: store, scheduler: scheduler, runner: runner, clock: world.engineClock,
                                ids: ids, fs: fs, dataDirectory: "/data")
        subscription = bus.subscribe(types: []) { [unowned self] e in self.onEvent(e) }
        var n = 0
        for j in given["jobs"]!.array! {
            n += 1
            let hex = String(n, radix: 16)
            let id = Id("00000000-0000-4000-8000-" + String(repeating: "0", count: 12 - hex.count) + hex)
            let name = j["name"]!.string!
            jobs.append((name, id))
            let kind = JobKind(rawValue: j["kind"]?.string ?? "videoDisc")!
            let title = j["title"]?.string ?? name
            let request = JobRequest(kind: kind, title: title, automatic: j["automatic"]?.bool == true, driveId: j["driveId"]?.string,
                                     mediaGeneration: j["mediaGeneration"]?.int)
            let plan = j["plan"].map { JobPlan(steps: FakeStep.strings($0).map { StepKind(rawValue: $0)! }) }
            let state = JobState(rawValue: j["state"]?.string ?? "queued")!
            let record = JobRecord(id: id, kind: kind, state: state, outcome: j["outcome"]?.string.flatMap(Outcome.init(rawValue:)),
                                   error: j["error"].map { BroError($0["code"]!.string!, $0["params"]?.members ?? []) },
                                   queue: Queue(rawValue: j["queue"]?.string ?? "acquisition")!, position: Int(j["position"]?.int ?? 0),
                                   driveId: request.driveId, mediaGeneration: request.mediaGeneration, automatic: request.automatic, title: title,
                                   request: request, plan: plan, createdAt: j["created"]?.string.flatMap(Instant.parse) ?? world.clock.now(),
                                   finishedAt: state == .finished ? world.clock.now() : nil)
            try await store.insert(record)
            var seq = 0
            for d in j["done"]?.array ?? [] {
                let k = StepKind(rawValue: d["kind"]!.string!)!
                try await store.save(StepRecord(jobId: id, seq: seq, kind: k, state: StepState(rawValue: d["state"]?.string ?? "succeeded")!, attempt: 1,
                                                startedAt: world.clock.now(), finishedAt: world.clock.now(),
                                                output: StepOutput(kind: k, data: d["output"] ?? .object([]),
                                                                   summary: d["summary"].map { BroMessage(MessageCode(rawValue: $0["code"]!.string!)!, $0["params"]?.members ?? []) },
                                                                   readErrors: Int(d["readErrors"]?.int ?? 0)),
                                                error: d["error"].map { BroError($0["code"]!.string!, $0["params"]?.members ?? []) },
                                                checkpoint: d["checkpoint"]))
                seq += 1
            }
            if j["intent"]?.bool == true { store.openIntent(id) }
            if let unit = j["unit"]?.string { store.storeUnit(id, UnitState(rawValue: unit)!) }
            if let log = j["log"] {
                let lines = FakeStep.strings(log).enumerated().map { n, c in
                    LogLineDto(seq: Int64(n + 1), t: world.clock.now(), severity: .info, code: MessageCode(rawValue: c)!, params: [])
                }
                try fs.createDirectory("/data/jobs/\(id.value)", parentsMustExist: false)
                try fs.writeAtomically("/data/jobs/\(id.value)/log.jsonl", bytes: JobLog.encode(lines), mode: 0o600)
            }
            if let text = j["logText"]?.string {
                try fs.createDirectory("/data/jobs/\(id.value)", parentsMustExist: false)
                try fs.writeAtomically("/data/jobs/\(id.value)/log.jsonl", bytes: Array(text.utf8), mode: 0o600)
            }
        }
    }

    func id(_ name: String) -> Id { jobs.first { $0.name == name }?.id ?? Id("00000000-0000-4000-8000-0000000000ff") }

    /// Every event; a jobFinished gives the job's outcome, a jobChanged must come after its record is stored.
    func onEvent(_ e: EngineEvent) {
        events.append(e)
        guard e.type == .jobChanged || e.type == .jobFinished, let job = e.data["job"], let id = job["id"]?.string else { return }
        if e.type == .jobChanged && store.job(Id(id)) == nil { announcedUnstored.append(id) }
        if e.type == .jobFinished, let name = jobs.first(where: { $0.id.value == id })?.name {
            outcomes[name] = Decided(outcome: Outcome(rawValue: job["outcome"]!.string!)!,
                                     error: job["error"].map { BroError($0["code"]!.string!, $0["params"]?.members ?? []) })
        }
    }
    /// A job's name; "?" for one no op named (a job the engine made that the case didn't expect), so the case fails by name.
    func name(_ id: String) -> String { jobs.first { $0.id.value == id }?.name ?? "?" }

    /// The engine has settled: everything still going on is parked (a step at a closed gate, a step waiting for an
    /// answer), and every job that was run has finished or waits (for resources, an answer, a gate).
    func check() -> Bool {
        let atGate = gates.parked()
        var parked = atGate.count
        var ok = true
        for (name, id) in jobs where !stopped && started.contains(name) && outcomes[name] == nil {
            let last = events.last { ($0.type == .jobChanged || $0.type == .jobFinished) && $0.data["job"]?["id"]?.string == id.value }
            let state = last?.data["job"]?["state"]?.string
            if state == nil && service { continue } // a created job not stored yet (its insert held): the activity tells
            if state == "awaitingDecision" && !gates.isCancelled(id) {
                parked += 1
            } else if !(state == "waitingForResources" || state == "blocked" || atGate.contains(id)) {
                ok = false
            }
        }
        return ok && world.executor.activity == parked + store.heldInserts + store.heldLoads
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
        case "cancel" where !service:
            gates.cancelled(id(arg.string!))
            runner.cancel(id(arg.string!))
        case "decide" where !service:
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
        case "holdInserts":
            store.holdInserts()
        case "releaseInserts":
            store.releaseInserts()
        case "holdLoads":
            store.holdLoads(id(arg.string!))
        case "releaseLoads":
            store.releaseLoads()
        case "steps":
            for (kind, behaviour) in arg.members! { behaviours[StepKind(rawValue: kind)!] = behaviour }
        default:
            guard service else { throw FixtureError("unknown op \(name)") }
            let call = try serviceCall(name, arg)
            let index = calls.count
            let task = Task { @EngineActor in
                do throws(BroError) { try await call() } catch { self.callErrors[index] = error.code }
                self.callsEnded.insert(index)
            }
            calls.append((op, task))
        }
    }

    /// A JobService op: what it calls. A job it creates gets its name now (the ids count).
    func serviceCall(_ name: String, _ arg: JsonValue) throws -> @EngineActor () async throws(BroError) -> Void {
        let job = arg.string.map { id($0) } ?? arg["job"]?.string.map { id($0) }
        let s = jobService!
        switch name {
        case "start":
            for (n, i) in jobs where store.job(i).map({ $0.state != .finished }) == true { started.insert(n) }
            return { () async throws(BroError) in try await s.start() }
        case "create":
            let request = JobRequest(kind: JobKind(rawValue: arg["kind"]?.string ?? "videoDisc")!, title: arg["title"]?.string ?? arg["as"]!.string!,
                                     automatic: arg["automatic"]?.bool == true, driveId: arg["driveId"]?.string, mediaGeneration: arg["mediaGeneration"]?.int)
            register(arg["as"]!.string!)
            return { () async throws(BroError) in _ = try await s.create(request) }
        case "retry":
            if let a = arg["as"]?.string { register(a) }
            let from = arg["fromStep"]?.string.flatMap(StepKind.init(rawValue:))
            return { () async throws(BroError) in _ = try await s.retry(job!, fromStep: from) }
        case "keepApart":
            return { () async throws(BroError) in _ = try await s.keepApart(job!) }
        case "cancel":
            gates.cancelled(job!)
            return { () async throws(BroError) in try await s.cancel(job!) }
        case "decide":
            let decision = arg["decisionId"]!.string!, answer = arg["answer"]!.string!
            return { () async throws(BroError) in try await s.decide(job!, decisionId: decision, answer: answer) }
        case "move":
            let by = Int(arg["by"]!.int!)
            return { () async throws(BroError) in try await s.move(job!, by: by) }
        case "startNow":
            return { () async throws(BroError) in try await s.startNow(job!) }
        case "stop":
            stopped = true
            for (_, i) in jobs { gates.cancelled(i) } // the shutdown cancels every live job's token
            return { () async throws(BroError) in await s.stop() }
        case "detail":
            return { [unowned self] () async throws(BroError) in
                do throws(BroError) {
                    let json = JobDetailDto.json(try await s.job(job!))
                    details.append(.object([("job", .string(detailText(json))), ("steps", json["steps"] ?? .null), ("files", json["files"] ?? .null),
                                            ("commands", json["commands"] ?? .null), ("logBytes", json["logBytes"] ?? .null)]))
                } catch {
                    details.append(.object([("error", .string(error.code))]))
                    throw error
                }
            }
        case "query":
            let q = JobQueryDto(states: FakeStep.strings(arg["states"]).map { JobState(rawValue: $0)! },
                                outcomes: FakeStep.strings(arg["outcomes"]).map { Outcome(rawValue: $0)! },
                                kind: arg["kind"]?.string.flatMap(JobKind.init(rawValue:)), queue: arg["queue"]?.string.flatMap(Queue.init(rawValue:)),
                                driveId: arg["driveId"]?.string, cursor: arg["cursor"]?.string, limit: Int(arg["limit"]?.int ?? Int64(JobQueryDto.defaultLimit)))
            return { [unowned self] () async throws(BroError) in
                do throws(BroError) {
                    let page = try await s.query(q)
                    queries.append(.object([("items", .array(page.items.map { .string(itemText($0)) })), ("next", page.next.map { .string($0) } ?? .null),
                                            ("total", .integer(Int64(page.total)))]))
                } catch {
                    queries.append(.object([("error", .string(error.code))]))
                    throw error
                }
            }
        case "log":
            let after = arg["after"]!.int!
            return { [unowned self] () async throws(BroError) in
                do throws(BroError) {
                    let lines = try await s.log(job!, after: after)
                    logs.append(.array(lines.map { .string("\($0.seq) \($0.code.rawValue)") }))
                } catch {
                    logs.append(.object([("error", .string(error.code))]))
                    throw error
                }
            }
        default:
            throw FixtureError("unknown op \(name)")
        }
    }

    func register(_ job: String) {
        jobs.append((job, ids.peek()))
        started.insert(job)
    }

    /// A JobDetail's job as the case writes it, read from its JSON.
    func detailText(_ j: JsonValue) -> String {
        var text = "\(name(j["id"]?.string ?? "")) \(j["state"]?.string ?? "-")"
        if let o = j["outcome"]?.string { text += " " + o }
        if let p = j["position"]?.int { text += " position \(p)" }
        if let step = j["progress"]?["step"]?.string { text += " step " + step }
        return text
    }

    func itemText(_ j: JobDto) -> String {
        var text = "\(name(j.id.value)) \(j.state.rawValue)"
        if let o = j.outcome { text += " " + o.rawValue }
        if let p = j.position { text += " position \(p)" }
        if let progress = j.progress { text += " step " + progress.step.rawValue }
        return text
    }

    /// Every JobService op has ended as it expects (its expectError, else no error; maybeError: that error or none, for
    /// ops that race).
    func checkCalls() async throws {
        for (i, call) in calls.enumerated() {
            let until = Date().addingTimeInterval(10)
            while !callsEnded.contains(i) {
                if Date() > until { throw FixtureError("the op hasn't finished: \(call.op)") }
                try await Task.sleep(nanoseconds: 1_000_000)
            }
            let arg = call.op.members!.first!.value
            if let maybe = arg["maybeError"]?.string, callErrors[i] == nil || callErrors[i] == maybe { continue }
            try Fixtures.same(arg["expectError"]?.string, callErrors[i], "error of \(call.op)")
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
                if let position = job["position"]?.int { text += " position \(position)" }
                if let f = job["progress"]?["currentFraction"]?.double { text += " progress \(Int((f * 100).rounded()))" }
                if let b = job["blockedBy"]?["code"]?.string { text += " blockedBy " + b }
                if let d = job["decision"]?["id"]?.string { text += " decision " + d }
                lines[name(job["id"]!.string!), default: []].append(text)
            case .stepFinished:
                lines[name(e.data["job"]!.string!), default: []].append("step \(e.data["kind"]!.string!) \(e.data["state"]!.string!)")
            case .jobLog:
                for l in e.data["lines"]!.array! { lines[name(e.data["jobId"]!.string!), default: []].append("log " + l["code"]!.string!) }
            case .jobFinished:
                let job = e.data["job"]!
                var text = "finished " + job["outcome"]!.string!
                if let code = job["error"]?["code"]?.string { text += " " + code }
                lines[name(job["id"]!.string!), default: []].append(text)
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

/// shared/fixtures/engine/pipeline-runner.cases.json, job-service.cases.json (the same harness with a JobService over the
/// runner) and job-planner.cases.json.
struct PipelineRunnerTests {
    static func check(_ id: String, _ given: JsonValue, _ expect: JsonValue, service: Bool = false) async throws {
        let h = try await PipelineHarness(given, service: service)
        for op in given["ops"]!.array! {
            try await h.run(op)
            try await h.settle()
        }
        try await h.checkCalls()
        if service {
            try Fixtures.same("", await h.announcedUnstored.joined(separator: " "), "jobs announced before they were stored")
            if let q = expect["queries"] { try Fixtures.same(q, JsonValue.array(await h.queries), "queries") }
            if let l = expect["logs"] { try Fixtures.same(l, JsonValue.array(await h.logs), "logs") }
            if let d = expect["details"] { try Fixtures.same(d, JsonValue.array(await h.details), "details") }
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
            let got: [String: JsonValue] = [
                "state": .string(r.state.rawValue), "outcome": r.outcome.map { .string($0.rawValue) } ?? .null, "queue": .string(r.queue.rawValue),
                "position": .integer(Int64(r.position)), "automatic": .bool(r.automatic), "mediaGeneration": r.mediaGeneration.map { .integer($0) } ?? .null,
                "title": .string(r.title), "driveId": r.driveId.map { .string($0) } ?? .null,
                "plan": r.plan.map { .array($0.steps.map { .string($0.rawValue) }) } ?? .null,
                "mode": r.mode.map { .string($0) } ?? .null, "libraryId": r.plan?.libraryId.map { .string($0) } ?? .null,
            ]
            for (key, value) in want.members! { try Fixtures.same(value, got[key]!, "record \(key) of \(name)") }
        }
        for (name, want) in expect["finishing"]?.members ?? [] {
            try Fixtures.same(want, h.behaviours.finishing(await h.id(name)).map { .string($0) } ?? .null, "finishing of \(name)")
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
        for (name, want) in expect["checkpoints"]?.members ?? [] {
            let steps = h.store.stepsOf(await h.id(name))
            for (kind, checkpoint) in want.members! {
                try Fixtures.same(checkpoint, steps.last { $0.kind.rawValue == kind }!.checkpoint ?? .null, "checkpoint of \(name)'s \(kind)")
            }
        }
        for (name, want) in expect["readErrors"]?.members ?? [] {
            let steps = h.store.stepsOf(await h.id(name))
            for (kind, count) in want.members! {
                try Fixtures.same(count.int.map { Int($0) }, steps.last { $0.kind.rawValue == kind }!.output!.readErrors, "read errors of \(name)'s \(kind)")
            }
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

    @Test func theSharedJobServiceCasesPass() async throws {
        let doc = try Fixtures.json("engine/job-service.cases.json")
        var failures: [String] = []
        for c in doc["cases"]!.array! {
            let id = c["id"]!.string!
            do { try await PipelineRunnerTests.check(id, c["given"]!, c["expect"]!, service: true) } catch { failures.append("\(id): \(error)") }
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    @Test func theSharedPlannerCasesPass() throws {
        let failures = try Fixtures.runCases("engine/job-planner.cases.json") { _, given, expect in
            let r = given["request"]!
            let request = JobRequest(kind: JobKind(rawValue: r["kind"]!.string!)!, title: r["title"]!.string!, automatic: r["automatic"]?.bool == true,
                                     driveId: r["driveId"]?.string, sessionId: r["sessionId"]?.string.map { Id($0) })
            try Fixtures.same(FakeStep.strings(expect["steps"]), JobPlanner.initial(request).map(\.rawValue), "steps")
            if let queue = expect["queue"] { try Fixtures.same(queue.string, JobPlanner.queue(request.kind).rawValue, "queue") }
            return true
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }
}
