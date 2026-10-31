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
/// engine log; English messages and app version "test". Build, then `start`.
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

    /// LibraryService's crash hook (H's crash points): throw there to stop a commit as a killed engine would.
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
    /// The store under the engine (a harness peeks at it; the engine owns it).
    var store: any Store { sqlite! }
    private var sqlite: SqliteStore?
    private var stopped = true

    /// `makemkvconVersion`: what the locator reports as makemkvcon's version; `mkvmerge`: whether the locator finds it;
    /// `environment`: the BROMELIA_SECRET_* variables the root hands ConfigService; `cores`: the processor count in
    /// EngineFacts; `streamCapacity`: the bus's stream capacity (a small one makes a feed's reader fall behind).
    init(makemkvcon: Bool = true, makemkvconVersion: String? = nil, mkvmerge: Bool = true, environment: [String: String] = [:], cores: Int = 8,
         streamCapacity: Int = 1024) async throws {
        folder = Fixtures.temporaryDirectory + "bromelia-root-" + UUID().uuidString
        try FileManager.default.createDirectory(atPath: folder, withIntermediateDirectories: true)
        fs.anyVolume = VolumeInfo(id: "vol", freeBytes: 1_000_000_000_000, fsType: "apfs", caseSensitive: false)
        launcher = ScriptedProcessLauncher(files: fs)
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
        let clock = EngineClock(clock: self.clock, executor: executor)
        let problems = ProblemService(bus: bus, clock: self.clock)
        let config = ConfigService(executor: executor, bus: bus, problems: problems, fs: fs, keystore: keystore, secretFiles: secretFiles,
                                   kv: store.kv(), path: Self.configPath, environment: environment)
        let scheduler = Scheduler(executor: executor, bus: bus, limits: SchedulerLimits())
        let libraries = LibraryService(executor: executor, bus: bus, problems: problems, scheduler: scheduler, config: config, clock: clock, fs: fs,
                                       libraries: store.libraries(), units: store.units(), catalog: store.catalog(),
                                       crash: { [self] (point: String) throws(BroError) in try crash?(point) })
        let makemkv = MakemkvTool(launcher: launcher, fs: fs, isolation: isolation, locator: locator)
        let drives = DriveService(executor: executor, bus: bus, problems: problems, config: config, clock: clock, monitor: self.drives,
                                  control: self.drives, makemkv: makemkv, repository: store.drives())
        let makemkvSetup = MakemkvService(fs: fs, config: config, dataDir: Self.root + "/home/.MakeMKV", userSettings: nil, home: Self.root + "/home")
        let registry = StepRegistry.standard()
        let tools = ToolSet(makemkv: makemkv, matroska: MkvToolNix(launcher: launcher, fs: fs, locator: locator, workDirectory: Self.dataDir + "/tmp"))
        let environment = StepEnvironment(tools: tools, fs: fs, clock: self.clock, drives: self.drives, library: libraries, catalog: libraries,
                                          config: config, driveLookup: drives, makemkvSetup: makemkvSetup, dataDirectory: Self.dataDir,
                                          messages: MessageTextRenderer(EnglishMessages.catalog), appVersion: "test")
        let runner = PipelineRunner(executor: executor, bus: bus, scheduler: scheduler, store: store, registry: registry,
                                          environment: environment, clock: clock)
        let jobs = JobService(executor: executor, bus: bus, store: store, scheduler: scheduler, runner: runner, clock: clock, ids: ids, fs: fs,
                                    dataDirectory: Self.dataDir)
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

    /// LocalEngine.stop (once).
    func stop() async throws {
        if stopped { return }
        stopped = true
        try await engine.stop()
    }

    /// Stops the engine (when running), then builds a new one on the same database and files and starts it.
    func restart() async throws {
        try await stop()
        try await build()
        try await start()
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

    /// Stops the engine when it is running (at most 40 s), closes the store and deletes the temporary folder.
    func dispose() async {
        if !stopped {
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

/// A flag set from any thread.
final class Flag: @unchecked Sendable {
    private let lock = NSLock()
    private var value = false

    var isSet: Bool { lock.withLock { value } }

    func set() { lock.withLock { value = true } }
}
