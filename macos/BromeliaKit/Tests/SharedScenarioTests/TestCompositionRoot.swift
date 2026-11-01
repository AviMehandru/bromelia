import BroAdapters
import BroDomain
import BroEngine
import BroFoundation
import BroPorts
import BroTestKit
import BroTestSupport
import Foundation

/// Builds the engine the real composition root builds (decision 174): the same services, `StepRegistry.standard()` and
/// LocalEngine, over the test kit's fakes. A manual clock; files in memory under `root` (the data folder is `dataDir`);
/// SQLite on a file in a temporary folder of the real disk, so `restart` reopens the same database; scripted processes
/// under the real MakemkvTool and MkvToolNix; the fake drives; an in-memory keystore pair; counting ids; a recording
/// engine log; English messages and app version "test". Build, then `start`. A scenario runner passes its own launcher
/// for the tools, and kills the engine at a crash point (`kill`, then `restart`).
final class TestCompositionRoot: @unchecked Sendable {
    static let root = "/root"
    static let dataDir = root + "/data"
    static let configPath = dataDir + "/config.json"
    static let makemkvcon = "/tools/makemkvcon"
    static let mkvmerge = "/tools/mkvmerge"

    /// The temporary folder on disk that holds bromelia.db.
    let folder: String
    let clock = ManualClock()
    let fs = MemoryFileSystem()
    let launcher: ScriptedProcessLauncher
    /// What MakemkvTool and MkvToolNix start their processes with: the initializer's launcher, else `launcher`.
    let toolLauncher: any ProcessLauncher
    let isolation = RecordingIsolation()
    let locator: FixedLocator
    let drives = FakeDrives()
    let keystore = MemoryKeystore(systemStore: true)
    let secretFiles = MemoryKeystore(systemStore: false)
    let ids = CountingIds()
    let log = RecordingEngineLog()
    let environment: [String: String]
    private let cores: Int
    private let streamCapacity: Int
    private let lock = NSLock()
    private var onReleaseHook: (@Sendable () -> Void)?
    private var crashHook: (@Sendable (String) throws(BroError) -> Void)?

    /// Called when an engine's release runs (after its executor stopped), before the store closes.
    var onRelease: (@Sendable () -> Void)? {
        get { lock.withLock { onReleaseHook } }
        set { lock.withLock { onReleaseHook = newValue } }
    }

    /// The crash hook of LibraryService (commit.*) and PipelineRunner (beforeStep:/afterStep:<kind>): throw there (what
    /// `kill` returns) to stop the engine as a killed one would.
    var crash: (@Sendable (String) throws(BroError) -> Void)? {
        get {
            lock.lock()
            defer { lock.unlock() }
            return crashHook
        }
        set {
            lock.lock()
            crashHook = newValue
            lock.unlock()
        }
    }

    // The graph built last.
    private(set) var executor: EngineExecutor!
    private(set) var bus: EventBus!
    private(set) var libraries: LibraryService!
    private(set) var engine: LocalEngine!
    /// The subscription that keeps `RootState.published` (the bus's last seq) for a kill.
    private var seen: Subscription?
    /// The store under the engine (a harness peeks at it; the engine owns it).
    var store: any Store { sqlite! }
    private var sqlite: SqliteStore?
    private var jobs: JobService?
    private var stopped = true
    private let state = RootState()

    /// How long the last `stop` took, in real time (seconds).
    var lastStop: TimeInterval { state.get { $0.lastStop } }
    /// The bus's last seq when the engine was killed: what it publishes after that never happened.
    var killedSeq: Int64? { state.get { $0.killedSeq } }
    /// The last seq the bus published (its internal events too).
    var publishedSeq: Int64 { state.get { $0.published } }
    /// A stop that takes this long hit LocalEngine's cap (G8 N1: a restart after it fails the case).
    static let stopCap: TimeInterval = 30
    /// How long a killed engine has to go quiet before its executor is stopped.
    static let killCap: TimeInterval = 10

    /// `makemkvconVersion`: what the locator reports as makemkvcon's version; `mkvmerge`: whether the locator finds it;
    /// `environment`: the BROMELIA_SECRET_* variables the root hands ConfigService; `cores`: the processor count in
    /// EngineFacts; `streamCapacity`: the bus's stream capacity (a small one makes a feed's reader fall behind); `launcher`:
    /// what the tools start their processes with (a scenario's scripted tools; nil: `launcher`).
    init(makemkvcon: Bool = true, makemkvconVersion: String? = nil, mkvmerge: Bool = true, environment: [String: String] = [:], cores: Int = 8,
         streamCapacity: Int = 1024, launcher toolLauncher: (any ProcessLauncher)? = nil) async throws {
        folder = Fixtures.temporaryDirectory + "bromelia-root-" + UUID().uuidString
        try FileManager.default.createDirectory(atPath: folder, withIntermediateDirectories: true)
        fs.anyVolume = VolumeInfo(id: "vol", freeBytes: 1_000_000_000_000, fsType: "apfs", caseSensitive: false)
        launcher = ScriptedProcessLauncher(files: fs)
        self.toolLauncher = toolLauncher ?? launcher
        locator = FixedLocator(makemkvcon: makemkvcon ? Self.makemkvcon : nil, mkvmerge: mkvmerge ? Self.mkvmerge : nil,
                               makemkvconVersion: makemkvconVersion)
        self.environment = environment
        self.cores = cores
        self.streamCapacity = streamCapacity
        fs.putFolder(Self.dataDir)
        try await build()
    }

    /// A new graph on the same database, files, clock, processes and drives; the event seqs go on from the last engine's
    /// (a host starts each engine above the last one's). The store is opened and migrated here, as the real root does.
    @EngineActor
    private func build() async throws {
        let firstSeq: Int64
        if let bus { firstSeq = bus.lastSeq + 1 } else { firstSeq = 1 }
        let store = try SqliteStore.open(folder + "/bromelia.db", clock: clock)
        try await store.migrate()
        sqlite = store
        let executor = EngineExecutor()
        let bus = EventBus(clock: clock, firstSeq: firstSeq, streamCapacity: streamCapacity)
        let state = state
        state.set { $0.published = firstSeq - 1 }
        seen = bus.subscribe(types: []) { e in state.set { $0.published = max($0.published, e.seq) } }
        let clock = EngineClock(clock: self.clock, executor: executor)
        let problems = ProblemService(bus: bus, clock: self.clock)
        let config = ConfigService(executor: executor, bus: bus, problems: problems, fs: fs, keystore: keystore, secretFiles: secretFiles,
                                   kv: store.kv(), path: Self.configPath, environment: environment)
        let scheduler = Scheduler(executor: executor, bus: bus, limits: SchedulerLimits())
        let libraries = LibraryService(executor: executor, bus: bus, problems: problems, scheduler: scheduler, config: config, clock: clock, fs: fs,
                                       libraries: store.libraries(), units: store.units(), catalog: store.catalog(),
                                       crash: { [self] (point: String) throws(BroError) in try crash?(point) })
        let makemkv = MakemkvTool(launcher: toolLauncher, fs: fs, isolation: isolation, locator: locator)
        let drives = DriveService(executor: executor, bus: bus, problems: problems, config: config, clock: clock, monitor: self.drives,
                                  control: self.drives, makemkv: makemkv, repository: store.drives())
        let makemkvSetup = MakemkvService(fs: fs, config: config, dataDir: Self.root + "/home/.MakeMKV", userSettings: nil, home: Self.root + "/home")
        let registry = StepRegistry.standard()
        let tools = ToolSet(makemkv: makemkv, matroska: MkvToolNix(launcher: toolLauncher, fs: fs, locator: locator, workDirectory: Self.dataDir + "/tmp"))
        let environment = StepEnvironment(tools: tools, fs: fs, clock: self.clock, drives: self.drives, library: libraries, catalog: libraries,
                                          config: config, driveLookup: drives, makemkvSetup: makemkvSetup, dataDirectory: Self.dataDir,
                                          messages: MessageTextRenderer(EnglishMessages.catalog), appVersion: "test")
        let runner = PipelineRunner(executor: executor, bus: bus, scheduler: scheduler, store: store, registry: registry,
                                    environment: environment, clock: clock, crash: { [self] (point: String) throws(BroError) in try crash?(point) })
        let jobs = JobService(executor: executor, bus: bus, store: store, scheduler: scheduler, runner: runner, clock: clock, ids: ids, fs: fs,
                                    dataDirectory: Self.dataDir)
        self.jobs = jobs
        let recovery = RecoveryService(executor: executor, bus: bus, problems: problems, store: store, registry: registry, library: libraries,
                                             config: config, clock: clock, fs: fs, isolation: isolation,
                                             renderer: MessageTextRenderer(EnglishMessages.catalog), dataDirectory: Self.dataDir)
        let facts = EngineFacts(version: "test", pid: 4242, platform: "macos", dataDir: Self.dataDir, cores: cores)
        self.executor = executor
        self.bus = bus
        self.libraries = libraries
        engine = LocalEngine(executor: executor, bus: bus, clock: clock, problems: problems, config: config, scheduler: scheduler,
                             libraries: libraries, drives: drives, jobs: jobs, recovery: recovery, locator: locator, log: log, facts: facts,
                             release: { [self] in
                                 onRelease?()
                                 store.close()
                             })
    }

    /// LocalEngine.start; the engine counts as running from now on, even when start fails.
    func start() async throws {
        stopped = false
        try await engine.start()
    }

    /// LocalEngine.stop (once), timed in real time (`lastStop`).
    func stop() async throws {
        if stopped { return }
        stopped = true
        let began = Date()
        defer { state.set { $0.lastStop = Date().timeIntervalSince(began) } }
        try await engine.stop()
    }

    /// Stops the engine (when running), then builds a new one on the same database and files and starts it. After a `kill`
    /// it finishes the kill instead of stopping. A stop that hit the 30 s cap fails it (G8 N1: only a host's exit is safe
    /// after that), as does a killed engine that doesn't go quiet within 10 s.
    func restart() async throws {
        if state.get({ $0.killedAt }) != nil {
            try await finishKill()
        } else {
            try await stop()
            if lastStop >= Self.stopCap { throw CaseFailure(String(format: "the stop took %.1f s: it hit the 30 s cap", lastStop)) }
        }
        try await build()
        try await start()
    }

    /// Kills the engine at a crash point (decision 240): called at the point, it freezes the files (no write lands from now
    /// on), stops JobService ahead of whatever the point unwinds into (so the runner stores nothing more) and notes the bus's
    /// last seq; it returns what the point must throw. PipelineRunner's points (beforeStep:, afterStep:) are on the executor,
    /// and the runner stops itself as the hook throws; the others (inStep:, commit.*) are on a step's worker, which waits
    /// here until JobService.stop has begun on the executor (Swift doesn't order a spawned task before the worker's result,
    /// which comes back through the mailbox). The store stays open until the old executor is quiet (`restart`).
    func kill(_ point: String) -> BroError {
        fs.freeze()
        state.set {
            $0.killedAt = point
            $0.killedSeq = $0.published
        }
        let jobs = jobs!
        let begun = Flag()
        executor.post {
            Task { @EngineActor in
                begun.set()
                await jobs.stop() // sets the runner's stopping flag before its first suspension
            }
        }
        if !(point.hasPrefix("beforeStep:") || point.hasPrefix("afterStep:")) {
            let deadline = Date().addingTimeInterval(Self.killCap)
            while !begun.isSet && Date() < deadline { usleep(1000) }
        }
        return BroError("internal.unexpected", [("detail", .string("killed at " + point))])
    }

    /// Once the killed engine's executor is quiet (at most 10 s): its executor stops, its store closes, its timers are
    /// dropped and the files thaw.
    private func finishKill() async throws {
        let point = state.get { $0.killedAt } ?? ""
        let deadline = Date().addingTimeInterval(Self.killCap)
        while executor.activity > 0 {
            if Date() > deadline { throw CaseFailure("the engine killed at \(point) didn't go quiet within 10 s") }
            try await Task.sleep(nanoseconds: 5_000_000)
        }
        await executor.stop()
        sqlite?.close()
        clock.cancelAll()
        fs.thaw()
        state.set { $0.killedAt = nil }
        stopped = true
    }

    /// The library folder's volume goes away (its files are kept aside), and LibraryService checks at once (owner answer
    /// 10: no OS event reports a library's volume).
    func unmount(_ path: String) async throws {
        try fs.rename(path, to: Self.root + "/.unmounted" + path.replacingOccurrences(of: "/", with: "_"))
        await check()
    }

    /// The volume comes back, and LibraryService checks at once.
    func mount(_ path: String) async throws {
        try fs.rename(Self.root + "/.unmounted" + path.replacingOccurrences(of: "/", with: "_"), to: path)
        await check()
    }

    private func check() async {
        let executor = executor!, libraries = libraries!
        await withCheckedContinuation { (done: CheckedContinuation<Void, Never>) in
            let posted = executor.post {
                executor.spawn {
                    await libraries.check()
                    done.resume()
                }
            }
            if !posted { done.resume() }
        }
    }

    /// Stops the engine when it is running (at most 40 s), or finishes a kill (at most 15 s), closes the store and deletes
    /// the temporary folder.
    func dispose() async {
        if state.get({ $0.killedAt }) != nil {
            try? await finishKill()
        } else if !stopped {
            let engine = engine!
            let ended = Flag()
            Task {
                try? await engine.stop()
                ended.set()
            }
            let deadline = Date().addingTimeInterval(40)
            while !ended.isSet && Date() < deadline { try? await Task.sleep(nanoseconds: 5_000_000) }
            stopped = true
        }
        sqlite?.close()
        try? FileManager.default.removeItem(atPath: folder)
    }
}

/// What the root keeps across threads: the last stop's time, the kill, the last seq the bus published.
final class RootState: @unchecked Sendable {
    struct Values {
        var lastStop: TimeInterval = 0
        var killedAt: String?
        var killedSeq: Int64?
        var published: Int64 = 0
    }

    private let lock = NSLock()
    private var values = Values()

    func get<T>(_ read: (Values) -> T) -> T { lock.withLock { read(values) } }

    func set(_ change: (inout Values) -> Void) { lock.withLock { change(&values) } }
}

/// A flag set from any thread.
final class Flag: @unchecked Sendable {
    private let lock = NSLock()
    private var value = false

    var isSet: Bool { lock.withLock { value } }

    func set() { lock.withLock { value = true } }

    func reset() { lock.withLock { value = false } }
}
