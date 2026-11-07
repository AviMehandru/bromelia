import BroDomain
import BroFoundation
import BroPorts
import Foundation

/// Implements `EngineAPI` over the services (plan §13.1, §15.1). Callable from any thread (never from the executor's):
/// each call hops onto the EngineExecutor. The composition root builds it, then the host calls `start()` and calls
/// nothing else before that completes. After `stop()` every call answers server.stopping.
/// shared/fixtures/engine/local-engine.cases.json.
@EngineActor
public final class LocalEngine: EngineAPI {
    /// How long stop waits for the jobs to settle and the executor to go idle; the host exits anyway after it.
    nonisolated static let settleCap = Swift.Duration.seconds(30)
    /// The finished jobs a snapshot carries, newest first.
    nonisolated static let finishedInSnapshot = 50

    /// What stop's first part gave back, read off the executor while stop waits: the error, or that the shutdown ended.
    private final class Stopping: @unchecked Sendable {
        let lock = NSLock()
        var error: BroError?
        var done = false
    }

    private nonisolated let executor: EngineExecutor
    private let bus: EventBus
    private nonisolated let clock: EngineClock
    private let problems: ProblemService
    private let configService: ConfigService
    private let scheduler: Scheduler
    private let libraryService: LibraryService
    private let driveService: DriveService
    private let jobService: JobService
    private let recovery: RecoveryService
    private nonisolated let locator: any ToolLocator
    private nonisolated let log: any EngineLog
    private nonisolated let facts: EngineFacts
    private nonisolated let release: @Sendable () -> Void
    private var feeds: [EventFeed] = []
    private var limits: Subscription?
    private var startedAt = Instant(unixMilliseconds: 0)
    private var stopping = false

    /// `release` is called once stop has stopped the executor: the root closes the store there.
    public nonisolated init(executor: EngineExecutor, bus: EventBus, clock: EngineClock, problems: ProblemService, config: ConfigService,
                            scheduler: Scheduler, libraries: LibraryService, drives: DriveService, jobs: JobService, recovery: RecoveryService,
                            locator: any ToolLocator, log: any EngineLog, facts: EngineFacts, release: @escaping @Sendable () -> Void) {
        self.executor = executor
        self.bus = bus
        self.clock = clock
        self.problems = problems
        self.configService = config
        self.scheduler = scheduler
        self.libraryService = libraries
        self.driveService = drives
        self.jobService = jobs
        self.recovery = recovery
        self.locator = locator
        self.log = log
        self.facts = facts
        self.release = release
    }

    // MARK: Start and stop

    /// The startup order (plan §15.1): the configuration (a failed load fails start, and nothing else has started),
    /// recovery (recovery.summary into the engine log), the configuration's limits to the Scheduler (and on every
    /// configChanged), the libraries, the drives, then the jobs recovery left queued.
    public nonisolated func start() async throws(BroError) {
        try await on { () throws(BroError) in
            self.startedAt = self.clock.now()
            try await self.configService.load()
            if let summary = try await self.recovery.recover() { self.log.log(summary) }
            self.applyLimits()
            self.limits = self.bus.subscribe(types: [.configChanged]) { _ in self.applyLimits() }
            try await self.libraryService.start()
            try await self.driveService.start()
            try await self.jobService.start()
        }
    }

    /// The stop order: from now on every call answers server.stopping; the feeds close and the intake stops (drives,
    /// libraries); the runner shuts every live job down storing nothing more, so the store is what a killed engine
    /// leaves; then, once no job is live and the executor is idle (at most 30 s, in real time), the executor stops and
    /// the root's release runs.
    public nonisolated func stop() async throws(BroError) {
        let state = Stopping()
        Task {
            do throws(BroError) {
                try await self.on { () throws(BroError) in
                    self.stopping = true
                    for feed in self.feeds { feed.close() }
                    self.feeds = []
                    self.limits?.cancel()
                    self.limits = nil
                    self.driveService.stop()
                    self.libraryService.stop()
                    await self.jobService.stop()
                }
            } catch {
                state.lock.withLock { state.error = error }
            }
            state.lock.withLock { state.done = true }
        }
        let deadline = ContinuousClock.now + Self.settleCap
        while ContinuousClock.now < deadline {
            let (done, error) = state.lock.withLock { (state.done, state.error) }
            if let error { throw error }
            if done { break }
            try? await Task.sleep(for: .milliseconds(5))
        }
        while executor.activity > 0 && ContinuousClock.now < deadline { try? await Task.sleep(for: .milliseconds(5)) }
        await executor.stop()
        release()
    }

    /// A door outside EngineAPI: a job for the request (an ISO or a folder needs SessionService otherwise), for the
    /// scenario runner and the real-ISO test. Retired when openSession/ripSession come.
    public nonisolated func startJob(_ request: JobRequest) async throws(BroError) -> JobDto {
        try await on { () throws(BroError) in try await self.jobService.create(request) }
    }

    // MARK: System

    public nonisolated func system() async throws(BroError) -> SystemStatusDto {
        try await on { () throws(BroError) in try await self.status() }
    }

    // MARK: Drives

    public nonisolated func drives() async throws(BroError) -> [DriveDto] {
        try await on { self.driveService.drives() }
    }

    public nonisolated func eject(_ driveId: String) async throws(BroError) {
        try await on { () throws(BroError) in try await self.driveService.eject(driveId) }
    }

    public nonisolated func closeTray(_ driveId: String) async throws(BroError) {
        try await on { () throws(BroError) in try await self.driveService.closeTray(driveId) }
    }

    public nonisolated func closeAllTrays() async throws(BroError) {
        try await on { await self.driveService.closeAll() }
    }

    public nonisolated func rip(_ driveId: String, options: RipOptionsDto) async throws(BroError) -> JobDto {
        try await on { () throws(BroError) in
            guard let drive = self.driveService.drives().first(where: { $0.id == driveId && $0.connected }) else {
                throw Self.failure(.driveNotFound, [("driveId", .string(driveId))])
            }
            guard let medium = drive.medium else { throw Self.failure(.driveNoDisc) }
            if drive.busy { throw Self.failure(.driveBusy) }
            if medium.content == .audio || medium.content == .data { throw Self.failure(.discNotVideo, [("drive", .string(drive.name))]) }
            if options.titles != nil { throw Self.failure(.sessionNotOpen) }
            var library = options.library
            if let folder = options.outputFolder {
                guard let id = self.libraryService.libraries().first(where: { Self.samePath($0.path, folder) })?.id else {
                    throw Self.failure(.libraryNotAttached, [("path", .string(folder))])
                }
                library = id
            }
            var chosen: [(key: String, value: JsonValue)] = []
            if let mode = options.mode { chosen.append(("mode", .string(mode.rawValue))) }
            if let profile = options.profile { chosen.append(("profile", .string(profile))) }
            if let library { chosen.append(("library", .string(library))) }
            let title = medium.label.isEmpty ? drive.name : medium.label
            return try await self.jobService.create(JobRequest(kind: .videoDisc, title: title, automatic: false, driveId: drive.id,
                                                               mediaGeneration: medium.generation, options: .object(chosen)))
        }
    }

    // MARK: Jobs

    public nonisolated func jobs(_ query: JobQueryDto) async throws(BroError) -> JobPageDto {
        try await on { () throws(BroError) in try await self.jobService.query(query) }
    }

    public nonisolated func job(_ jobId: Id) async throws(BroError) -> JobDetailDto {
        try await on { () throws(BroError) in try await self.jobService.job(jobId) }
    }

    public nonisolated func jobLog(_ jobId: Id, after: Int64) async throws(BroError) -> [LogLineDto] {
        try await on { () throws(BroError) in try await self.jobService.log(jobId, after: after) }
    }

    public nonisolated func cancel(_ jobId: Id) async throws(BroError) {
        try await on { () throws(BroError) in try await self.jobService.cancel(jobId) }
    }

    public nonisolated func retry(_ jobId: Id, fromStep: StepKind?) async throws(BroError) -> JobDto {
        try await on { () throws(BroError) in try await self.jobService.retry(jobId, fromStep: fromStep) }
    }

    public nonisolated func keepApart(_ jobId: Id) async throws(BroError) {
        try await on { () throws(BroError) in _ = try await self.jobService.keepApart(jobId) }
    }

    public nonisolated func decide(_ jobId: Id, decisionId: String, answer: String) async throws(BroError) {
        try await on { () throws(BroError) in try await self.jobService.decide(jobId, decisionId: decisionId, answer: answer) }
    }

    public nonisolated func move(_ jobId: Id, by: Int) async throws(BroError) {
        try await on { () throws(BroError) in try await self.jobService.move(jobId, by: by) }
    }

    public nonisolated func startNow(_ jobId: Id) async throws(BroError) {
        try await on { () throws(BroError) in try await self.jobService.startNow(jobId) }
    }

    // MARK: Libraries

    public nonisolated func libraries() async throws(BroError) -> [LibraryDto] {
        try await on { self.libraryService.libraries() }
    }

    /// The id is the name (else the folder's last part) made into an EntryId, with -2, -3 … when the configuration has
    /// it already. LibraryService writes the marker, then the configuration gets {id, name, path} at its current
    /// version; if that save fails the marker is removed and the save's error returned, so a retry doesn't meet
    /// library.alreadyAttached. The path of a library the configuration already has (the user confirming a folder
    /// without its marker), compared byte for byte: the marker is written under that library's id and name, and the
    /// configuration is left as it is.
    public nonisolated func attachLibrary(_ path: String, name: String) async throws(BroError) -> LibraryDto {
        try await on { () throws(BroError) in
            let current = self.configService.current()
            if let configured = Config.libraries(current).first(where: { Array($0.path.utf8) == Array(path.utf8) }) {
                let marked = try await self.libraryService.attach(configured.id, path: path, name: configured.name)
                return self.libraryService.libraries().first(where: { $0.id == configured.id }) ?? marked
            }
            let display = name.isEmpty ? Self.lastPart(path) : name
            let id = Self.newLibraryId(display, taken: Config.libraries(current).map(\.id))
            let attached = try await self.libraryService.attach(id, path: path, name: name)
            let entry = JsonValue.object([("id", .string(id)), ("name", .string(attached.name)), ("path", .string(path))])
            let libraries = (current.document["libraries"]?.array ?? []) + [entry]
            let members = (current.document.members ?? []).filter { $0.key != "libraries" } + [("libraries", .array(libraries))]
            do throws(BroError) {
                _ = try await self.configService.update(.object(members), expectedVersion: self.configService.version())
            } catch {
                try await self.libraryService.removeMarker(id)
                throw error
            }
            return self.libraryService.libraries().first(where: { $0.id == id }) ?? attached
        }
    }

    // MARK: Configuration

    public nonisolated func config() async throws(BroError) -> ConfigEnvelope {
        try await on { () throws(BroError) in try await self.configService.envelope() }
    }

    public nonisolated func updateConfig(_ doc: JsonValue, version: Int64) async throws(BroError) -> ConfigEnvelope {
        try await on { () throws(BroError) in try await self.configService.update(doc, expectedVersion: version) }
    }

    public nonisolated func validateConfig(_ doc: JsonValue) async throws(BroError) -> [Issue] {
        try await on { () throws(BroError) in try self.configService.validate(doc) }
    }

    public nonisolated func setSecret(_ name: String, value: String) async throws(BroError) {
        try await on { () throws(BroError) in try await self.configService.setSecret(name, value: value) }
    }

    public nonisolated func exportConfig() async throws(BroError) -> JsonValue {
        try await on { JsonValue.parse(self.configService.export()) ?? .null }
    }

    // MARK: Events

    /// With `after` the feed replays from it (one the bus can't replay starts with a resync, which the feed answers
    /// with a snapshot); without it the first event is a snapshot whose seq is the last event's before the snapshot was
    /// gathered, and the events published while it was gathered follow it as deltas.
    public nonisolated func events(after: Int64?, jobLog: [Id]) async throws(BroError) -> EventFeed {
        try await on { () throws(BroError) in
            let stream: EventStream
            var first: EngineEvent?
            if let after {
                stream = self.bus.stream(after: after, types: [])
            } else {
                let snapshot = try await self.snapshot()
                if self.stopping { throw Self.failure(.serverStopping) }
                first = snapshot
                stream = self.bus.stream(after: snapshot.seq, types: [])
            }
            let feed = EventFeed(stream: stream, jobLog: jobLog, first: first) { () throws(BroError) in
                try await self.on { () throws(BroError) in try await self.snapshot() }
            }
            self.feeds.removeAll { $0.isClosed }
            self.feeds.append(feed)
            return feed
        }
    }

    // MARK: Inside

    /// Runs `work` on the executor; its result or failure comes back to the caller. server.stopping once stop has
    /// begun.
    private nonisolated func on<T: Sendable>(_ work: @escaping @EngineActor @Sendable () async throws(BroError) -> T) async throws(BroError) -> T {
        let executor = executor
        let result = await withCheckedContinuation { (done: CheckedContinuation<Result<T, BroError>, Never>) in
            let posted = executor.post {
                executor.spawn {
                    do throws(BroError) {
                        if self.stopping { throw Self.failure(.serverStopping) }
                        done.resume(returning: .success(try await work()))
                    } catch {
                        done.resume(returning: .failure(error))
                    }
                }
            }
            if !posted { done.resume(returning: .failure(Self.failure(.serverStopping))) }
        }
        return try result.get()
    }

    private func applyLimits() { scheduler.updateLimits(SchedulerLimits.of(configService.current(), cores: facts.cores)) }

    private func status() async throws(BroError) -> SystemStatusDto {
        let locator = locator
        let tools = try await executor.worker { [locator.locate(.makemkvcon), locator.locate(.mkvmerge)] }
        let makemkv = tools[0]
        return SystemStatusDto(facts: facts, startedAt: startedAt, makemkvFound: makemkv.path != nil, makemkvVersion: makemkv.version,
                               tools: tools, problems: problems.problems(), libraries: libraryService.libraries(),
                               configVersion: configService.version(), eventSeq: bus.lastSeq)
    }

    /// The snapshot event (never published): its seq is the last event's when gathering began.
    private func snapshot() async throws(BroError) -> EngineEvent {
        let seq = bus.lastSeq
        var unfinished: [JobDto] = []
        let states: [JobState] = [.queued, .waitingForResources, .running, .blocked, .awaitingDecision]
        var cursor: String?
        repeat {
            let page = try await jobService.query(JobQueryDto(states: states, cursor: cursor, limit: JobQueryDto.maxLimit))
            unfinished += page.items
            cursor = page.next
        } while cursor != nil
        let finished = try await jobService.query(JobQueryDto(states: [.finished], limit: Self.finishedInSnapshot))
        var system = try await status()
        system.eventSeq = seq
        let snapshot = SnapshotDto(system: system, drives: driveService.drives(), jobs: unfinished + finished.items, queues: scheduler.queues(),
                                   problems: problems.problems(), libraries: libraryService.libraries(), configVersion: configService.version())
        return EngineEvent(seq: seq, at: clock.now(), type: .snapshot, data: SnapshotDto.json(snapshot))
    }

    private nonisolated static func failure(_ code: MessageCode, _ params: [(key: String, value: JsonValue)] = []) -> BroError {
        BroError(MessageCode.wire(code), params)
    }

    private nonisolated static func samePath(_ a: String, _ b: String) -> Bool { trimmed(a) == trimmed(b) }

    private nonisolated static func trimmed(_ path: String) -> Substring {
        var s = Substring(path)
        while let last = s.last, last == "/" || last == "\\" { s = s.dropLast() }
        return s
    }

    private nonisolated static func lastPart(_ path: String) -> String {
        let t = trimmed(path)
        guard let i = t.lastIndex(where: { $0 == "/" || $0 == "\\" }) else { return String(t) }
        return String(t[t.index(after: i)...])
    }

    /// The name as an EntryId (common.json: letters, digits, '.', '_', '-'; at most 64): ASCII letters and digits
    /// lowercased, every run of anything else one '-', no '-' at either end, at most 60 characters; "library" when
    /// nothing is left. Then -2, -3 … until no id of `taken` has it (ignoring case).
    private nonisolated static func newLibraryId(_ name: String, taken: [String]) -> String {
        var id = ""
        for c in name.unicodeScalars {
            if ("a"..."z").contains(c) || ("0"..."9").contains(c) {
                id.unicodeScalars.append(c)
            } else if ("A"..."Z").contains(c) {
                id.unicodeScalars.append(Unicode.Scalar(c.value + 32)!)
            } else if !id.isEmpty && id.last != "-" {
                id.append("-")
            }
        }
        var stem = Substring(id)
        while stem.last == "-" { stem = stem.dropLast() }
        if stem.count > 60 {
            stem = stem.prefix(60)
            while stem.last == "-" { stem = stem.dropLast() }
        }
        let base = stem.isEmpty ? "library" : String(stem)
        let lower = Set(taken.map { $0.lowercased() }) // EntryIds are ASCII
        var candidate = base
        var n = 2
        while lower.contains(candidate) {
            candidate = "\(base)-\(n)"
            n += 1
        }
        return candidate
    }
}
