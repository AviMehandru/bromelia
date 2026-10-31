using Bromelia.Adapters;
using Bromelia.Domain;
using Bromelia.Engine;
using Bromelia.Foundation;
using Bromelia.Ports;
using Bromelia.TestKit;

namespace Bromelia.Scenarios.Tests;

/// <summary>
/// Builds the engine the real composition root builds (decision 174): the same services, <see cref="StepRegistry.Standard"/>
/// and LocalEngine, over the test kit's fakes. A manual clock; files in memory under <see cref="Root"/> (the data folder
/// is <see cref="DataDir"/>); SQLite on a file in a temporary folder of the real disk, so <see cref="Restart"/> reopens
/// the same database; scripted processes under the real MakemkvTool and MkvToolNix; the fake drives; an in-memory
/// keystore pair; counting ids; a recording engine log; English messages and app version "test". Build, then
/// <see cref="Start"/>.
/// </summary>
public sealed class TestCompositionRoot : IDisposable
{
    public const string Root = "/root";
    public const string DataDir = Root + "/data";
    public const string ConfigPath = DataDir + "/config.json";
    public const string Makemkvcon = "/tools/makemkvcon";
    public const string Mkvmerge = "/tools/mkvmerge";

    /// <summary>The temporary folder on disk that holds bromelia.db.</summary>
    public readonly string Folder;
    public readonly ManualClock Clock = new();
    public readonly MemoryFileSystem Fs = new() { AnyVolume = new VolumeInfo("vol", 1_000_000_000_000, "apfs", false) };
    public readonly ScriptedProcessLauncher Launcher;
    public readonly RecordingIsolation Isolation = new();
    public readonly FixedLocator Locator;
    public readonly FakeDrives Drives = new();
    public readonly MemoryKeystore Keystore = new(systemStore: true);
    public readonly MemoryKeystore SecretFiles = new(systemStore: false);
    public readonly CountingIds Ids = new();
    public readonly RecordingEngineLog Log = new();
    public readonly IReadOnlyDictionary<string, string> Environment;
    readonly int _cores;
    readonly int _streamCapacity;
    /// <summary>Called when an engine's release runs (after its executor stopped), before the store closes.</summary>
    public Action? OnRelease;
    /// <summary>LibraryService's crash hook (H's crash points): throw there to stop a commit as a killed engine would.</summary>
    public Action<string>? Crash;

    // The graph built last.
    public EngineExecutor Executor { get; private set; } = null!;
    public EventBus Bus { get; private set; } = null!;
    public LibraryService Libraries { get; private set; } = null!;
    public LocalEngine Engine { get; private set; } = null!;
    /// <summary>The store under the engine (a harness peeks at it; the engine owns it).</summary>
    public IStore Store => _store!;
    SqliteStore? _store;
    bool _stopped = true;

    /// <param name="makemkvconVersion">What the locator reports as makemkvcon's version.</param>
    /// <param name="mkvmerge">Whether the locator finds mkvmerge.</param>
    /// <param name="environment">The BROMELIA_SECRET_* variables the root hands ConfigService.</param>
    /// <param name="cores">The processor count in EngineFacts.</param>
    /// <param name="streamCapacity">The bus's stream capacity (a small one makes a feed's reader fall behind).</param>
    public TestCompositionRoot(bool makemkvcon = true, string? makemkvconVersion = null, bool mkvmerge = true,
        IReadOnlyDictionary<string, string>? environment = null, int cores = 8, int streamCapacity = 1024)
    {
        Folder = Path.Combine(Path.GetTempPath(), "bromelia-root-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(Folder);
        Launcher = new ScriptedProcessLauncher(Fs);
        Locator = new FixedLocator(makemkvcon ? Makemkvcon : null, mkvmerge ? Mkvmerge : null, makemkvconVersion);
        _streamCapacity = streamCapacity;
        Environment = environment ?? new Dictionary<string, string>();
        _cores = cores;
        Fs.PutFolder(DataDir);
        Build();
    }

    /// <summary>A new graph on the same database, files, clock, processes and drives; the event seqs go on from the last
    /// engine's (a host starts each engine above the last one's). The store is opened and migrated here, as the real
    /// root does.</summary>
    void Build()
    {
        long firstSeq = Bus != null ? Bus.LastSeq + 1 : 1;
        var store = SqliteStore.Open(Path.Combine(Folder, "bromelia.db"), Clock);
        Task.Run(store.Migrate).GetAwaiter().GetResult();
        _store = store;
        var log = Log;
        var executor = new EngineExecutor("bromelia-engine",
            e => log.Log(new BroMessage(MessageCode.InternalUnexpected, Severity.Error, ("detail", JsonValue.Of(e.ToString())))));
        var bus = new EventBus(Clock, firstSeq, streamCapacity: _streamCapacity);
        var clock = new EngineClock(Clock, executor);
        var problems = new ProblemService(bus, Clock);
        var config = new ConfigService(executor, bus, problems, Fs, Keystore, SecretFiles, store.Kv(), ConfigPath, Environment);
        var scheduler = new Scheduler(executor, bus, new SchedulerLimits());
        var libraries = new LibraryService(executor, bus, problems, scheduler, config, clock, Fs, store.Libraries(), store.Units(), store.Catalog(),
            point => Crash?.Invoke(point));
        var makemkv = new MakemkvTool(Launcher, Fs, Isolation, Locator);
        var drives = new DriveService(executor, bus, problems, config, clock, Drives, Drives, makemkv, store.Drives());
        var makemkvSetup = new MakemkvService(Fs, config, Root + "/home/.MakeMKV", null, Root + "/home");
        var registry = StepRegistry.Standard();
        var environment = new StepEnvironment(new ToolSet(makemkv, new MkvToolNix(Launcher, Fs, Locator, DataDir + "/tmp")), Fs, Clock, Drives,
            libraries, libraries, config, drives, makemkvSetup, DataDir, new MessageTextRenderer(EnglishMessages.Catalog), "test");
        var runner = new PipelineRunner(executor, bus, scheduler, store, registry, environment, clock);
        var jobs = new JobService(executor, bus, store, scheduler, runner, clock, Ids, Fs, DataDir);
        var recovery = new RecoveryService(executor, bus, problems, store, registry, libraries, config, clock, Fs, Isolation,
            new MessageTextRenderer(EnglishMessages.Catalog), DataDir);
        var facts = new EngineFacts("test", 4242, "macos", DataDir, _cores);
        Executor = executor;
        Bus = bus;
        Libraries = libraries;
        Engine = new LocalEngine(executor, bus, clock, problems, config, scheduler, libraries, drives, jobs, recovery, Locator, Log, facts,
            () =>
            {
                OnRelease?.Invoke();
                store.Close();
            });
    }

    /// <summary>LocalEngine.start; the engine counts as running from now on, even when start fails.</summary>
    public Task Start()
    {
        _stopped = false;
        return Engine.Start();
    }

    /// <summary>LocalEngine.stop (once).</summary>
    public async Task Stop()
    {
        if (_stopped) return;
        _stopped = true;
        await Engine.Stop();
    }

    /// <summary>Stops the engine (when running), then builds a new one on the same database and files and starts it.</summary>
    public async Task Restart()
    {
        await Stop();
        Build();
        await Start();
    }

    /// <summary>The library folder's volume goes away (its files are kept aside), and LibraryService checks at once
    /// (owner answer 10: no OS event reports a library's volume).</summary>
    public Task Unmount(string path)
    {
        Fs.Rename(path, Root + "/.unmounted" + path.Replace('/', '_'));
        return Check();
    }

    /// <summary>The volume comes back, and LibraryService checks at once.</summary>
    public Task Mount(string path)
    {
        Fs.Rename(Root + "/.unmounted" + path.Replace('/', '_'), path);
        return Check();
    }

    Task Check()
    {
        var done = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Executor.Post(async () =>
        {
            await Libraries.Check();
            done.SetResult();
        });
        return done.Task;
    }

    public void Dispose()
    {
        try
        {
            if (!_stopped) Task.Run(Stop).Wait(TimeSpan.FromSeconds(40));
        }
        catch (AggregateException) { }
        _store?.Close();
        try { Directory.Delete(Folder, recursive: true); }
        catch (IOException) { }
    }
}
