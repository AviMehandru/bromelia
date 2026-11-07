import BroAdapters
import BroDomain
import BroEngine
import BroFoundation
import BroPorts
import Foundation

/// Builds the real object graph (plan §17): ports → adapters → services → LocalEngine; nothing else constructs adapters
/// or services. TestCompositionRoot (SharedScenarioTests) builds the same graph over fakes. Nothing runs until
/// LocalEngine.start; LocalEngine closes the store once it has stopped.
public enum CompositionRoot {
    /// The app version, until the release workflow (phase 6) stamps one.
    public static let appVersion = "1.0.0"

    /// Opens and migrates the store, builds every adapter, service, the runner and LocalEngine.
    @EngineActor
    public static func build(_ options: RootOptions) async throws(BroError) -> LocalEngine {
        let data = options.dataDir
        let configPath = options.configPath ?? data + "/config.json"
        let secrets = data + "/secrets"

        let clock = SystemClock()
        let fs = PlatformFileSystem()
        let launcher = PlatformProcessLauncher()
        let keystore = PlatformKeystore(fallbackDir: secrets, service: "Bromelia", systemStore: options.systemKeystore)
        let secretFiles = PlatformKeystore(fallbackDir: secrets, service: "Bromelia", systemStore: false)
        if !fs.exists(data) { try fs.createDirectory(data, parentsMustExist: false) }
        let store = try SqliteStore.open(data + "/bromelia.db", clock: clock)
        do throws(BroError) {
            try await store.migrate()
        } catch {
            store.close()
            throw error
        }
        let searchPath = options.systemTools ? (options.environment["PATH"] ?? "").split(separator: ":").map(String.init) : []
        let locator = SystemToolLocator(fs: fs, configured: [:], candidates: options.systemTools ? PlatformToolPaths.candidates(options.home) : [:],
                                        names: PlatformToolPaths.names(), searchPath: searchPath, home: options.home)
        let isolation = HomeDirIsolation(fs: fs, layout: .macos)
        let tool = MakemkvTool(launcher: launcher, fs: fs, isolation: isolation, locator: locator)
        let makemkv: any Makemkv = options.drives ? tool : NoDriveScan(tool)
        let mkvtoolnix = MkvToolNix(launcher: launcher, fs: fs, locator: locator, workDirectory: data + "/tmp")
        let monitor: any DeviceMonitor = options.drives ? PlatformDeviceMonitor(clock: clock) : NoDeviceMonitor()
        let control: any DriveControl = options.drives ? PlatformDriveControl(launcher: launcher, clock: clock) : NoDriveControl()
        let ids = RandomIdGenerator()
        let renderer = MessageTextRenderer(EnglishMessages.catalog)
        let log = FileEngineLog(fs: fs, clock: clock, path: data + "/logs/engine.log")

        // No unexpected handler: Swift's executor actions can't throw.
        let executor = EngineExecutor()
        let bus = EventBus(clock: clock)
        let engineClock = EngineClock(clock: clock, executor: executor)
        let problems = ProblemService(bus: bus, clock: clock)
        let secretVariables = options.environment.filter { $0.key.hasPrefix("BROMELIA_SECRET_") }
        let config = ConfigService(executor: executor, bus: bus, problems: problems, fs: fs, keystore: keystore, secretFiles: secretFiles,
                                   kv: store.kv(), path: configPath, environment: secretVariables)
        let scheduler = Scheduler(executor: executor, bus: bus, limits: SchedulerLimits())
        let libraries = LibraryService(executor: executor, bus: bus, problems: problems, scheduler: scheduler, config: config, clock: engineClock,
                                       fs: fs, libraries: store.libraries(), units: store.units(), catalog: store.catalog())
        let drives = DriveService(executor: executor, bus: bus, problems: problems, config: config, clock: engineClock, monitor: monitor,
                                  control: control, makemkv: makemkv, repository: store.drives())
        // MakeMKV's folder and the settings.conf that holds its key.
        let makemkvFolder = options.home + "/Library/MakeMKV"
        let makemkvSetup = MakemkvService(fs: fs, config: config, dataDir: makemkvFolder, userSettings: makemkvFolder + "/settings.conf",
                                          home: options.home)
        let registry = StepRegistry.standard()
        let environment = StepEnvironment(tools: ToolSet(makemkv: makemkv, matroska: mkvtoolnix), fs: fs, clock: clock, drives: control,
                                          library: libraries, catalog: libraries, config: config, driveLookup: drives, makemkvSetup: makemkvSetup,
                                          dataDirectory: data, messages: renderer, appVersion: appVersion)
        let runner = PipelineRunner(executor: executor, bus: bus, scheduler: scheduler, store: store, registry: registry, environment: environment,
                                    clock: engineClock)
        let jobs = JobService(executor: executor, bus: bus, store: store, scheduler: scheduler, runner: runner, clock: engineClock, ids: ids,
                              fs: fs, dataDirectory: data)
        let recovery = RecoveryService(executor: executor, bus: bus, problems: problems, store: store, registry: registry, library: libraries,
                                       config: config, clock: engineClock, fs: fs, isolation: isolation, renderer: renderer, dataDirectory: data)
        let facts = EngineFacts(version: appVersion, pid: Int64(ProcessInfo.processInfo.processIdentifier), platform: "macos", dataDir: data,
                                cores: ProcessInfo.processInfo.activeProcessorCount)
        return LocalEngine(executor: executor, bus: bus, clock: engineClock, problems: problems, config: config, scheduler: scheduler,
                           libraries: libraries, drives: drives, jobs: jobs, recovery: recovery, locator: locator, log: log, facts: facts,
                           release: { store.close() })
    }
}
