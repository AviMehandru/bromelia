import BroDomain
import BroEngine
import BroFoundation
import BroPorts
import Foundation

/// A gate a thread waits at until it opens (a held crash point), from any thread.
final class Gate: @unchecked Sendable {
    private let condition = NSCondition()
    private var isOpen = false

    func open() {
        condition.lock()
        isOpen = true
        condition.broadcast()
        condition.unlock()
    }

    func reset() {
        condition.lock()
        isOpen = false
        condition.unlock()
    }

    /// Blocks the calling thread until the gate opens, at most `seconds`.
    func wait(seconds: TimeInterval) {
        let until = Date().addingTimeInterval(seconds)
        condition.lock()
        while !isOpen && condition.wait(until: until) {}
        condition.unlock()
    }
}

/// A scenario's step didn't hold: what was expected, and what was found.
struct ScenarioFailure: Error, CustomStringConvertible {
    let message: String

    init(_ expected: String, _ found: String) { message = expected + " / " + found }

    init(_ message: String) { self.message = message }

    var description: String { message }
}

/// One scenario played against LocalEngine over TestCompositionRoot (decisions 238–243): the world, the scripted tools,
/// the jobs the steps named, and every event of one feed opened at start and drained all along (reopened after each
/// restart, the seqs going on; what a killed engine publishes after the kill never happened). Each action step makes the
/// LocalEngine call of decision 238; an action the engine refuses fails the scenario. A failure reads
/// `<id> step <n> (<kind>): <what was expected> / <what was found>`.
final class ScenarioRun: @unchecked Sendable {
    /// How long any wait may take in real time (seconds).
    static let patience: TimeInterval = 10

    let id: String
    let scenario: JsonValue
    let world: ScenarioWorld
    let tools: ScenarioTools
    let root: TestCompositionRoot
    /// The jobs by the name a step gave them, in the order they were named.
    private(set) var jobNames: [String] = []
    private(set) var jobs: [String: Id] = [:]
    private let lock = NSLock()
    private var events: [EngineEvent] = []
    private var feed: EventFeed?
    private var draining: Task<Void, Never>?
    /// The seq after which the current feed's events never happened (a kill).
    private var cutoff: Int64?
    private var killAt: String?
    private var killedAt: String?
    private var holdAt: String?
    private let held = Flag()
    private let release = Gate()
    private(set) var stepNumber = 0
    private(set) var stepKind = "start"

    private init(id: String, scenario: JsonValue, world: ScenarioWorld, tools: ScenarioTools, root: TestCompositionRoot) {
        self.id = id
        self.scenario = scenario
        self.world = world
        self.tools = tools
        self.root = root
    }

    /// The root takes its launcher before the tools exist (they need its clock and files): this hands each start on.
    private final class Deferred: ProcessLauncher, @unchecked Sendable {
        private let lock = NSLock()
        private var target: ScenarioTools?

        func set(_ tools: ScenarioTools) { lock.withLock { target = tools } }

        func start(_ spec: ProcessSpec) throws(BroError) -> any RunningProcess {
            guard let tools = lock.withLock({ target }) else { throw BroError("internal.unexpected", [("detail", .string("no tools yet"))]) }
            return try tools.start(spec)
        }
    }

    private static func make(_ id: String, _ scenario: JsonValue) async throws -> ScenarioRun {
        let world = try ScenarioWorld(scenario)
        let deferred = Deferred()
        let root = try await TestCompositionRoot(makemkvcon: world.installed("makemkvcon"), mkvmerge: world.installed("mkvmerge"), launcher: deferred)
        do {
            let tools = try ScenarioTools(world: world, clock: root.clock, fs: root.fs)
            deferred.set(tools)
            try await world.prepare(root)
            let run = ScenarioRun(id: id, scenario: scenario, world: world, tools: tools, root: root)
            root.crash = { [unowned run] (point: String) throws(BroError) in try run.point(point) }
            tools.crash = { [unowned run] (point: String) throws(BroError) in try run.point(point) }
            return run
        } catch {
            await root.dispose()
            throw error
        }
    }

    /// Plays the scenario by id (or the one given); throws `ScenarioFailure` with the full text when it fails.
    static func play(_ id: String, _ given: JsonValue? = nil) async throws {
        let scenario = try given ?? ScenarioCatalog.load(id)
        let run: ScenarioRun
        do {
            run = try await make(id, scenario)
        } catch let f as ScenarioFailure {
            throw ScenarioFailure("\(id) given: \(f.message)")
        }
        do {
            try await run.playAll()
        } catch {
            await run.dispose()
            throw error
        }
        await run.dispose()
    }

    private func playAll() async throws {
        do {
            try await root.start()
            try await openFeed(after: nil)
        } catch let f as ScenarioFailure {
            throw failed(f)
        } catch {
            throw failed(ScenarioFailure("the engine starts", Self.describe(error)))
        }
        let steps = scenario["steps"]?.array ?? []
        arm(from: 0)
        for (i, step) in steps.enumerated() {
            stepNumber = i + 1
            guard let (kind, value) = step.members?.first else { throw failed(ScenarioFailure("a step", ScenarioRun.jsonText(step))) }
            stepKind = kind
            do {
                try await self.step(kind, value)
            } catch let f as ScenarioFailure {
                throw failed(f)
            } catch {
                throw failed(ScenarioFailure("\(kind) to succeed", Self.describe(error)))
            }
        }
        if let killAt = lock.withLock({ killAt }) {
            throw failed(ScenarioFailure("the kill at \(killAt) to be followed by a restart", "the end of the steps"))
        }
    }

    private func failed(_ f: ScenarioFailure) -> ScenarioFailure { ScenarioFailure("\(id) step \(stepNumber) (\(stepKind)): \(f.message)") }

    static func describe(_ e: any Error) -> String {
        if let b = e as? BroError { return "error " + jsonText(errorJson(b)) }
        return "\(type(of: e)): \(e)"
    }

    /// A value's JSON on one line.
    static func jsonText(_ v: JsonValue?) -> String {
        guard let v else { return "null" }
        var out = ""
        var space = false
        for line in v.description.split(separator: "\n", omittingEmptySubsequences: false) {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if trimmed.isEmpty { continue }
            if space { out += " " }
            out += trimmed
            space = true
        }
        return out
    }

    static func errorJson(_ e: BroError) -> JsonValue {
        e.params.isEmpty ? .object([("code", .string(e.code))]) : .object([("code", .string(e.code)), ("params", .object(e.params))])
    }

    // MARK: The steps

    private func step(_ kind: String, _ v: JsonValue) async throws {
        let e = root.engine!
        switch kind {
        case "expect":
            try await ScenarioExpect.check(self, v)
        case "wait":
            try await ScenarioExpect.check(self, .object([("when", v)]))
        case "rip":
            try await rip(v)
        case "insert":
            guard let disc = v["disc"] ?? world.disc else { throw ScenarioFailure("a disc to insert", "none") }
            try world.insert(root, v["drive"]?.string ?? "", disc)
            try await quiet() // the engine has handled the OS's event (its drive scan) before the next step
        case "remove":
            try world.remove(root, v["drive"]?.string ?? "")
            try await quiet()
        case "openTray":
            try world.openTray(root, v["drive"]?.string ?? "")
            try await quiet()
        case "eject":
            try await e.eject(try await driveId(v["drive"]?.string ?? ""))
        case "closeTray":
            try await e.closeTray(try await driveId(v["drive"]?.string ?? ""))
        case "keepApart":
            try await e.keepApart(try job(v.string ?? ""))
        case "cancel":
            try await e.cancel(try job(v.string ?? ""))
        case "decide":
            let name = v["job"]?.string ?? ""
            let id = try job(name)
            // The question may not be asked yet (the job was just started): wait for it, as `wait` would.
            if try await e.job(id).job.decision == nil {
                try await ScenarioExpect.when(self, .object([("job", .string(name)), ("state", .string("awaitingDecision"))]))
            }
            guard let decision = try await e.job(id).job.decision else { throw ScenarioFailure("job \(name) awaiting a decision", "none") }
            try await e.decide(id, decisionId: decision.id, answer: v["answer"]?.string ?? "")
        case "move":
            try await e.move(try job(v["job"]?.string ?? ""), by: Int(v["by"]?.int ?? 0))
        case "startNow":
            try await e.startNow(try job(v.string ?? ""))
        case "retry":
            if v["drive"] != nil || v["titles"] != nil || v["makemkvSettings"] != nil {
                throw ScenarioFailure("retry with only job, fromStep and as (phase 3)", ScenarioRun.jsonText(v))
            }
            let retried = try await e.retry(try job(v["job"]?.string ?? ""), fromStep: v["fromStep"]?.string.flatMap(StepKind.init(rawValue:)))
            if let name = v["as"]?.string { nameJob(name, retried.id) }
        case "advance":
            try await advance(Int64(((v.double ?? 0) * 1000).rounded()))
        case "kill":
            try await killed() // armed since the start or the last restart (arm)
        case "stop":
            try await stop(at: v["at"]?.string ?? "")
        case "restart":
            try await restart()
        case "unmount":
            try await root.unmount(try libraryPath(v.string ?? ""))
        case "mount":
            try await root.mount(try libraryPath(v.string ?? ""))
        case "attachLibrary":
            _ = try await e.attachLibrary(Self.fill(v["path"]?.string ?? ""), name: v["name"]?.string ?? "")
        case "config":
            let envelope = try await e.config()
            _ = try await e.updateConfig(ScenarioWorld.fill(ScenarioWorld.mergePatch(envelope.config.document, v)), version: envelope.version)
        case "tools":
            for (tool, script) in v.members ?? [] { try world.setScript(tool, script) }
        case "write":
            root.fs.put(Self.path(v["path"]?.string ?? ""), ScenarioWorld.fileBytes(v["text"] ?? v["bytes"] ?? .string("")))
        case "flip":
            let path = Self.path(v["path"]?.string ?? "")
            var bytes = try root.fs.read(path)
            bytes[Int(v["byte"]?.int ?? 0)] ^= 1
            root.fs.put(path, bytes)
        case "delete":
            try root.fs.remove(Self.path(v.string ?? ""))
        case "swapDisc":
            world.disc = try ScenarioCatalog.disc(v)
        case "open", "choose":
            throw ScenarioFailure("\(kind) (SessionService, phase 4)", "not built")
        case "cancelCountdown", "mounted":
            throw ScenarioFailure("\(kind) (AutomationService, phase 4)", "not built")
        case "api":
            throw ScenarioFailure("api (the Server, phase 4)", "not built")
        case "verify", "rescan", "testStep", "testNotification":
            throw ScenarioFailure("\(kind) (phase 5)", "not built")
        case "repair":
            throw ScenarioFailure("repair (phase 7)", "not built")
        default:
            throw ScenarioFailure("a known step", kind)
        }
    }

    private func nameJob(_ name: String, _ id: Id) {
        if jobs[name] == nil { jobNames.append(name) }
        jobs[name] = id
    }

    /// Decision 238: an ISO, folder or file through startJob (chosen titles as a session snapshot); a drive through rip, or
    /// startJob when the rip is automatic or has titles.
    private func rip(_ v: JsonValue) async throws {
        let e = root.engine!
        if v["session"] != nil { throw ScenarioFailure("rip.session (SessionService, phase 4)", "not built") }
        if v["tracks"] != nil { throw ScenarioFailure("rip.tracks (Transform, phase 5)", "not built") }
        var options: [(key: String, value: JsonValue)] = []
        if let mode = v["mode"]?.string { options.append(("mode", .string(mode))) }
        if let profile = v["profile"]?.string { options.append(("profile", .string(profile))) }
        if let folder = v["outputFolder"]?.string {
            let path = Self.fill(folder)
            guard let library = try await e.libraries().first(where: { Self.trimSlash($0.path) == Self.trimSlash(path) })?.id else {
                throw ScenarioFailure("rip to succeed", "error " + ScenarioRun.jsonText(ScenarioRun.errorJson(
                    BroError(MessageCode.wire(.libraryNotAttached), [("path", .string(path))]))))
            }
            options.append(("library", .string(library)))
        }
        let titles = v["titles"]?.array?.compactMap { $0.int.map { Int($0) } }
        let automatic = v["automatic"]?.bool == true
        let made: JobDto
        if let driveName = v["drive"]?.string {
            let driveId = try await driveId(driveName)
            if !automatic && titles == nil {
                made = try await e.rip(driveId, options: RipOptionsDto(mode: v["mode"]?.string.flatMap(RipMode.init(rawValue:)), profile: v["profile"]?.string,
                                                                       outputFolder: v["outputFolder"]?.string.map(Self.fill)))
            } else {
                guard let drive = try await e.drives().first(where: { $0.id == driveId }) else { throw ScenarioFailure("drive \(driveName) in drives()", "not listed") }
                let disc = world.discIn(try world.driveNamed(driveName))
                let details: JsonValue? = if let titles, let disc {
                    DiscRequest.encode(DiscRequest(source: .drive(driveId: driveId), session: try Self.snapshot(disc, titles)))
                } else {
                    nil
                }
                made = try await e.startJob(JobRequest(kind: .videoDisc, title: drive.medium?.label ?? disc?["volume"]?.string ?? drive.name, automatic: automatic,
                                                       driveId: driveId, mediaGeneration: drive.medium?.generation, options: .object(options), details: details))
            }
        } else {
            let source: DiscSource
            if v["iso"] != nil {
                source = .iso(path: ScenarioWorld.iso)
            } else if let f = v["folder"]?.string {
                source = .folder(path: Self.path(f))
            } else if let f = v["file"]?.string {
                source = .file(path: Self.path(f))
            } else {
                throw ScenarioFailure("rip.iso, drive, folder or file", ScenarioRun.jsonText(v))
            }
            guard let disc = world.disc else { throw ScenarioFailure("given.disc for rip", "none") }
            made = try await e.startJob(JobRequest(kind: .videoDisc, title: disc["volume"]?.string ?? "", automatic: automatic, options: .object(options),
                                                   details: DiscRequest.encode(DiscRequest(source: source, session: try titles.map { try Self.snapshot(disc, $0) }))))
        }
        nameJob(v["as"]?.string ?? "job\(jobs.count + 1)", made.id)
    }

    /// H1-1: chosen titles as SessionService will store them: the disc's listing and every title ticked or not.
    private static func snapshot(_ disc: JsonValue, _ titles: [Int]) throws -> SessionSnapshot {
        let listing = try ScenarioWorld.parsedListing(disc)
        return SessionSnapshot(listing: listing, choices: SessionChoices(titles: listing.titles.indices.map { TitleChoice(index: $0, selected: titles.contains($0)) }))
    }

    func job(_ name: String) throws -> Id {
        guard let id = jobs[name] else { throw ScenarioFailure("a job named " + name, "none") }
        return id
    }

    /// The engine's DriveId of a fake drive (by its device).
    func driveId(_ name: String) async throws -> String {
        let device = try world.driveNamed(name).device
        guard let id = try await root.engine.drives().first(where: { $0.device == device })?.id else {
            throw ScenarioFailure("drive \(name) (\(device)) in drives()", "not listed")
        }
        return id
    }

    private func libraryPath(_ id: String) throws -> String {
        try world.configuration()["libraries"]?.array?.first(where: { $0["id"]?.string == id })?["path"]?.string ?? Self.path(id)
    }

    /// A path the scenario gives: relative to the root, {root} filled in.
    static func path(_ relative: String) -> String {
        let p = fill(relative)
        return p.hasPrefix("/") ? p : TestCompositionRoot.root + "/" + p
    }

    static func fill(_ s: String) -> String { s.replacingOccurrences(of: "{root}", with: TestCompositionRoot.root) }

    static func trimSlash(_ path: String) -> String {
        var bytes = Array(path.utf8)
        while bytes.last == UInt8(ascii: "/") { bytes.removeLast() }
        return String(decoding: bytes, as: UTF8.self)
    }

    // MARK: Events

    private func openFeed(after: Int64?) async throws {
        let feed = try await root.engine.events(after: after, jobLog: [])
        lock.withLock {
            self.feed = feed
            cutoff = nil
        }
        draining = Task { [self] in
            do {
                while let ev = try await feed.next() {
                    if ev.type == .snapshot { continue }
                    lock.withLock {
                        if self.feed !== feed { return }
                        if let cut = cutoff, ev.seq > cut { return }
                        events.append(ev)
                    }
                }
            } catch {}
        }
    }

    /// The events so far (a copy).
    func allEvents() -> [EngineEvent] { lock.withLock { events } }

    /// Until the feed has delivered what the bus published (it is read on its own task, and leaves out the jobLog events,
    /// so it can't be matched by seq): no new event three times in a row, 5 ms apart.
    func catchUp() async {
        let deadline = Date().addingTimeInterval(Self.patience)
        var seen = 0, count = -1
        while seen < 3 && Date() < deadline {
            try? await Task.sleep(nanoseconds: 5_000_000)
            let now = lock.withLock { events.count }
            seen = now == count ? seen + 1 : 0
            count = now
        }
    }

    // MARK: Settling

    /// The executor's activity is only the parked workers, three times in a row: the parked processes' and, for each job
    /// awaiting a decision, its step waiting for the answer.
    func quiet() async throws {
        let deadline = Date().addingTimeInterval(Self.patience)
        var seen = 0
        while seen < 3 {
            if Date() > deadline {
                throw ScenarioFailure("the engine to go quiet",
                                      "activity \(root.executor.activity), parked processes \(tools.parked()), questions \(awaitingDecisions())")
            }
            try await Task.sleep(nanoseconds: 5_000_000)
            seen = root.executor.activity == tools.parked() + awaitingDecisions() ? seen + 1 : 0
        }
    }

    /// The jobs whose latest jobChanged or jobFinished says awaitingDecision.
    private func awaitingDecisions() -> Int {
        var states: [String: String?] = [:]
        for ev in allEvents() where ev.type == .jobChanged || ev.type == .jobFinished {
            if let id = ev.data["job"]?["id"]?.string { states[id] = ev.data["job"]?["state"]?.string }
        }
        return states.values.filter { $0 == "awaitingDecision" }.count
    }

    private static func settled(_ state: JobState) -> Bool {
        state == .finished || state == .blocked || state == .awaitingDecision || state == .waitingForResources
    }

    /// Decision 239: until the engine is quiet and every job has settled (finished, blocked, waiting for resources or
    /// awaiting a decision); while some job hasn't but the clock has a timer, the clock moves to it. A kill waiting for its
    /// point ends the wait once it has happened.
    func settle() async throws {
        let deadline = Date().addingTimeInterval(Self.patience)
        while true {
            if wasKilled() { return }
            try await quiet()
            if wasKilled() { return }
            let unsettled = try await allJobs().filter { !Self.settled($0.state) }
            if unsettled.isEmpty {
                try await quiet()
                if try await allJobs().allSatisfy({ Self.settled($0.state) }) {
                    await catchUp()
                    return
                }
                continue
            }
            guard Date() <= deadline, let due = root.clock.nextDue() else {
                throw ScenarioFailure("every job to settle", "stuck: " + unsettled.map { j in
                    "\(nameOf(j.id)) \(j.state.rawValue)" + (j.progress.map { " at " + $0.step.rawValue } ?? "")
                }.joined(separator: ", "))
            }
            root.clock.advanceNow(max(0, due.unixMilliseconds - root.clock.now().unixMilliseconds))
        }
    }

    private func wasKilled() -> Bool { lock.withLock { killedAt != nil } }

    func allJobs() async throws -> [JobDto] {
        var all: [JobDto] = []
        var cursor: String?
        repeat {
            let page = try await root.engine.jobs(JobQueryDto(cursor: cursor, limit: JobQueryDto.maxLimit))
            all += page.items
            cursor = page.next
        } while cursor != nil
        return all.sorted { a, b in
            if a.createdAt.unixMilliseconds != b.createdAt.unixMilliseconds { return a.createdAt.unixMilliseconds < b.createdAt.unixMilliseconds }
            return a.id.value.utf8.lexicographicallyPrecedes(b.id.value.utf8)
        }
    }

    func nameOf(_ id: Id) -> String { jobNames.first { jobs[$0] == id } ?? id.value }

    /// The clock moves to each due timer in turn (the engine going quiet after each), then to now + ms.
    private func advance(_ ms: Int64) async throws {
        let target = root.clock.now().unixMilliseconds + ms
        try await quiet()
        while let due = root.clock.nextDue(), due.unixMilliseconds <= target {
            root.clock.advanceNow(max(0, due.unixMilliseconds - root.clock.now().unixMilliseconds))
            try await quiet()
        }
        root.clock.advanceNow(max(0, target - root.clock.now().unixMilliseconds))
        try await quiet()
        await catchUp()
    }

    // MARK: Crash points, kill, stop, restart

    /// Arms the kill or stop point of the steps from `from` up to the next restart, so the engine stops there however soon
    /// it gets there (a job started by the step before a kill may reach its point before the kill step is played).
    private func arm(from: Int) {
        let steps = scenario["steps"]?.array ?? []
        lock.withLock {
            for step in steps.dropFirst(from) {
                guard let (kind, value) = step.members?.first else { continue }
                if kind == "restart" { return }
                if kind == "kill" { killAt = value["at"]?.string }
                if kind == "stop" { holdAt = value["at"]?.string }
            }
        }
    }

    /// inStep:<kind> is inStep:<kind>:1.
    private static func normal(_ point: String) -> String {
        point.hasPrefix("inStep:") && point.filter({ $0 == ":" }).count == 1 ? point + ":1" : point
    }

    /// Every crash point the engine (or a tool start) reaches: a kill there kills the engine; a stop there holds it until
    /// the stop has begun.
    private func point(_ point: String) throws(BroError) {
        if point.hasPrefix("beforeStep:") {
            tools.stepBegins(String(point.dropFirst(11)))
        } else if point.hasPrefix("afterStep:") {
            tools.stepBegins(nil)
        }
        let (kill, hold) = lock.withLock { () -> (Bool, Bool) in
            if let at = killAt, killedAt == nil, Self.normal(point) == Self.normal(at) {
                killedAt = point
                return (true, false)
            }
            if let at = holdAt, Self.normal(point) == Self.normal(at) {
                holdAt = nil
                return (false, true)
            }
            return (false, false)
        }
        if kill {
            tools.endAll()
            let thrown = root.kill(point)
            let seq = root.killedSeq
            lock.withLock { cutoff = seq }
            throw thrown
        }
        if hold {
            held.set()
            release.wait(seconds: 60)
        }
    }

    /// H1-6: the engine is held at the point, LocalEngine.stop begins, the point is released, the stop ends. A point never
    /// reached, or a stop that hits the 30 s cap, fails the scenario.
    private func stop(at point: String) async throws {
        let deadline = Date().addingTimeInterval(Self.patience)
        while !held.isSet {
            if Date() > deadline { break }
            try await Task.sleep(nanoseconds: 5_000_000)
            if !held.isSet, root.executor.activity == tools.parked(),
               try await allJobs().allSatisfy({ $0.state == .finished || $0.state == .blocked || $0.state == .awaitingDecision }), !held.isSet {
                break
            }
        }
        if !held.isSet {
            lock.withLock { holdAt = nil }
            throw ScenarioFailure("the engine to reach \(point)", "it never did")
        }
        let root = root
        let stopping = Task { try await root.stop() }
        let barrier = Flag()
        let executor = root.executor!
        Task {
            await executor.barrier()
            barrier.set()
        }
        let until = Date().addingTimeInterval(0.2)
        while !barrier.isSet && Date() < until { try await Task.sleep(nanoseconds: 2_000_000) }
        release.open()
        try await stopping.value
        lock.withLock { feed = nil }
        if root.lastStop >= TestCompositionRoot.stopCap {
            throw ScenarioFailure("the stop to end within 30 s", String(format: "%.1f s (the cap)", root.lastStop))
        }
    }

    /// Until the engine has died at the armed kill point (the engine runs, settling, until then); a point it never reaches
    /// fails the scenario.
    private func killed() async throws {
        if wasKilled() { return }
        try await settle()
        if !wasKilled() {
            let at = lock.withLock { killAt } ?? ""
            let jobs = try await allJobs().map { j in
                "\(nameOf(j.id)) \(j.state.rawValue) \(j.outcome?.rawValue ?? "") \(ScenarioRun.jsonText(j.error.map(ScenarioRun.errorJson)))"
            }
            throw ScenarioFailure("the engine to reach \(at)", "it never did; jobs " + jobs.joined(separator: ", ")
                                  + "; makemkvcon " + tools.calls("makemkvcon").joined(separator: " | "))
        }
    }

    /// After a kill: once the point was reached, the killed engine is finished off and a new one starts on the same database
    /// and files. Otherwise the engine settles, stops (a stop that hits the 30 s cap fails) and a new one starts. The feed
    /// reopens after the last seq the old bus had.
    private func restart() async throws {
        if lock.withLock({ killAt }) != nil {
            try await killed()
        } else if lock.withLock({ feed }) != nil {
            try await settle()
        }
        await catchUp()
        let after = root.publishedSeq
        let old = lock.withLock { () -> EventFeed? in
            let f = feed
            feed = nil
            return f
        }
        old?.close()
        try await root.restart()
        lock.withLock {
            killAt = nil
            killedAt = nil
        }
        held.reset()
        release.reset()
        tools.stepBegins(nil)
        try await openFeed(after: after)
        arm(from: stepNumber)
    }

    func dispose() async {
        release.open()
        lock.withLock { feed }?.close()
        await root.dispose()
        draining?.cancel()
    }
}
