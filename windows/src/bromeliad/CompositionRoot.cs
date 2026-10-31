using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Bromelia.Adapters;
using Bromelia.Adapters.Windows;
using Bromelia.Domain;
using Bromelia.Engine;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromeliad;

/// <summary>Builds the real object graph (plan §17, decision 175): ports → adapters → services → LocalEngine; nothing else
/// constructs adapters or services. TestCompositionRoot (Bromelia.Scenarios.Tests) builds the same graph over fakes.
/// Nothing runs until LocalEngine.Start; LocalEngine closes the store once it has stopped.</summary>
public static class CompositionRoot
{
    /// <summary>The app version: this project's &lt;Version&gt;.</summary>
    public static readonly string AppVersion = typeof(CompositionRoot).Assembly.GetName().Version!.ToString(3);

    /// <summary>Opens and migrates the store, builds every adapter, service, the runner and LocalEngine.</summary>
    public static LocalEngine Build(RootOptions options)
    {
        var data = options.DataDir;
        var configPath = options.ConfigPath ?? Path.Combine(data, "config.json");
        var secrets = Path.Combine(data, "secrets");

        var clock = new SystemClock();
        var fs = new PlatformFileSystem();
        var launcher = new PlatformProcessLauncher();
        var keystore = new PlatformKeystore(secrets, "Bromelia", systemStore: options.SystemKeystore);
        var secretFiles = new PlatformKeystore(secrets, "Bromelia", systemStore: false);
        if (!fs.Exists(data)) fs.CreateDirectory(data, parentsMustExist: false);
        var store = SqliteStore.Open(Path.Combine(data, "bromelia.db"), clock);
        try
        {
            Task.Run(store.Migrate).GetAwaiter().GetResult();
        }
        catch
        {
            store.Close();
            throw;
        }
        var locator = new SystemToolLocator(fs, new Dictionary<ToolKind, string>(),
            options.SystemTools ? PlatformToolPaths.Candidates(options.Home) : new Dictionary<ToolKind, IReadOnlyList<string>>(),
            PlatformToolPaths.Names(), options.SystemTools ? SearchPath(options.Environment) : Array.Empty<string>(), options.Home);
        var isolation = new RegistrySwapIsolation(fs, new CurrentUserMakemkvRegistry(), CatalogKeys(), Path.Combine(data, "run", "makemkv-registry.json"));
        var makemkv = new MakemkvTool(launcher, fs, isolation, locator);
        var mkvtoolnix = new MkvToolNix(launcher, fs, locator, Path.Combine(data, "tmp"));
        var monitor = new PlatformDeviceMonitor(clock);
        var control = new PlatformDriveControl(clock);
        var ids = new RandomIdGenerator();
        var renderer = new MessageTextRenderer(EnglishMessages.Catalog);
        var log = new FileEngineLog(fs, clock, Path.Combine(data, "logs", "engine.log"));

        var executor = new EngineExecutor("bromelia-engine",
            e => log.Log(new BroMessage(MessageCode.InternalUnexpected, Severity.Error, ("detail", JsonValue.Of(e.ToString())))));
        var bus = new EventBus(clock);
        var engineClock = new EngineClock(clock, executor);
        var problems = new ProblemService(bus, clock);
        var secretVariables = options.Environment.Where(kv => kv.Key.StartsWith("BROMELIA_SECRET_", StringComparison.Ordinal))
            .ToDictionary(kv => kv.Key, kv => kv.Value);
        var config = new ConfigService(executor, bus, problems, fs, keystore, secretFiles, store.Kv(), configPath, secretVariables);
        var scheduler = new Scheduler(executor, bus, new SchedulerLimits());
        var libraries = new LibraryService(executor, bus, problems, scheduler, config, engineClock, fs, store.Libraries(), store.Units(), store.Catalog());
        var drives = new DriveService(executor, bus, problems, config, engineClock, monitor, control, makemkv, store.Drives());
        // Today's paths (Bromelia.Core MakeMKV.cs): MakeMKV keeps its settings in the registry on Windows.
        var makemkvSetup = new MakemkvService(fs, config, "", null, options.Home);
        var registry = StepRegistry.Standard();
        var environment = new StepEnvironment(new ToolSet(makemkv, mkvtoolnix), fs, clock, control, libraries, libraries, config, drives,
            makemkvSetup, data, renderer, AppVersion);
        var runner = new PipelineRunner(executor, bus, scheduler, store, registry, environment, engineClock);
        var jobs = new JobService(executor, bus, store, scheduler, runner, engineClock, ids, fs, data);
        var recovery = new RecoveryService(executor, bus, problems, store, registry, libraries, config, engineClock, fs, isolation, renderer, data);
        var facts = new EngineFacts(AppVersion, System.Environment.ProcessId, "windows", data, System.Environment.ProcessorCount);
        return new LocalEngine(executor, bus, engineClock, problems, config, scheduler, libraries, drives, jobs, recovery, locator, log, facts,
            store.Close);
    }

    /// <summary>The folders of PATH (its name is any case on Windows).</summary>
    static IReadOnlyList<string> SearchPath(IReadOnlyDictionary<string, string> environment) =>
        environment.FirstOrDefault(kv => string.Equals(kv.Key, "PATH", StringComparison.OrdinalIgnoreCase)).Value is { } path
            ? path.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries)
            : Array.Empty<string>();

    /// <summary>Every key of shared/catalog/settings-catalog.json (embedded).</summary>
    static IReadOnlyCollection<string> CatalogKeys()
    {
        using var stream = typeof(CompositionRoot).Assembly.GetManifestResourceStream("Bromeliad.settings-catalog.json")!;
        using var bytes = new MemoryStream();
        stream.CopyTo(bytes);
        var keys = new List<string>();
        foreach (var section in JsonValue.Parse(bytes.ToArray())?["sections"]?.AsArray ?? new List<JsonValue>())
            foreach (var setting in section["settings"]?.AsArray ?? new List<JsonValue>())
                if (setting["key"]?.AsString is { } key) keys.Add(key);
        return keys;
    }
}
