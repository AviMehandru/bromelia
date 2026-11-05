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
/// <see cref="Start"/>. A scenario runner passes its own launcher for the tools, and kills the engine at a crash point
/// (<see cref="Kill"/>, then <see cref="Restart"/>).
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
    /// <summary>What MakemkvTool and MkvToolNix start their processes with: the constructor's launcher, else <see cref="Launcher"/>.</summary>
    public readonly IProcessLauncher ToolLauncher;
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
    /// <summary>The crash hook of LibraryService (commit.*) and PipelineRunner (beforeStep:/afterStep:&lt;kind&gt;): throw there (what
    /// <see cref="Kill"/> returns) to stop the engine as a killed one would.</summary>
    public Action<string>? Crash;
    /// <summary>How long the last <see cref="Stop"/> took, in real time.</summary>
    public TimeSpan LastStop { get; private set; }
    /// <summary>The bus's last seq when the engine was killed: what it publishes after that never happened.</summary>
    public long? KilledSeq { get; private set; }
    /// <summary>A stop that takes this long hit LocalEngine's cap (G8 N1: a restart after it fails the case).</summary>
    public static readonly TimeSpan StopCap = TimeSpan.FromSeconds(30);
    /// <summary>How long a killed engine has to go quiet before its executor is stopped.</summary>
    public static readonly TimeSpan KillCap = TimeSpan.FromSeconds(10);

    // The graph built last.
    public EngineExecutor Executor { get; private set; } = null!;
    public EventBus Bus { get; private set; } = null!;
    public LibraryService Libraries { get; private set; } = null!;
    public LocalEngine Engine { get; private set; } = null!;
    /// <summary>The store under the engine (a harness peeks at it; the engine owns it).</summary>
    public IStore Store => _store!;
    SqliteStore? _store;
    JobService? _jobs;
    bool _stopped = true;
    string? _killedAt;

    /// <param name="makemkvconVersion">What the locator reports as makemkvcon's version.</param>
    /// <param name="mkvmerge">Whether the locator finds mkvmerge.</param>
    /// <param name="environment">The BROMELIA_SECRET_* variables the root hands ConfigService.</param>
    /// <param name="cores">The processor count in EngineFacts.</param>
    /// <param name="streamCapacity">The bus's stream capacity (a small one makes a feed's reader fall behind).</param>
    /// <param name="launcher">What the tools start their processes with (a scenario's scripted tools); null:
    /// <see cref="Launcher"/>.</param>
    public TestCompositionRoot(bool makemkvcon = true, string? makemkvconVersion = null, bool mkvmerge = true,
        IReadOnlyDictionary<string, string>? environment = null, int cores = 8, int streamCapacity = 1024, IProcessLauncher? launcher = null)
    {
        Folder = Path.Combine(Path.GetTempPath(), "bromelia-root-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(Folder);
        Launcher = new ScriptedProcessLauncher(Fs);
        ToolLauncher = launcher ?? Launcher;
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
        var makemkv = new MakemkvTool(ToolLauncher, Fs, Isolation, Locator);
        var drives = new DriveService(executor, bus, problems, config, clock, Drives, Drives, makemkv, store.Drives());
        var makemkvSetup = new MakemkvService(Fs, config, Root + "/home/.MakeMKV", null, Root + "/home");
        var registry = StepRegistry.Standard();
        var environment = new StepEnvironment(new ToolSet(makemkv, new MkvToolNix(ToolLauncher, Fs, Locator, DataDir + "/tmp")), Fs, Clock, Drives,
            libraries, libraries, config, drives, makemkvSetup, DataDir, new MessageTextRenderer(EnglishMessages.Catalog), "test");
        var runner = new PipelineRunner(executor, bus, scheduler, store, registry, environment, clock, point => Crash?.Invoke(point));
        var jobs = new JobService(executor, bus, store, scheduler, runner, clock, Ids, Fs, DataDir);
        _jobs = jobs;
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

    /// <summary>LocalEngine.stop (once), timed in real time (<see cref="LastStop"/>).</summary>
    public async Task Stop()
    {
        if (_stopped) return;
        _stopped = true;
        var watch = System.Diagnostics.Stopwatch.StartNew();
        await Engine.Stop();
        LastStop = watch.Elapsed;
    }

    /// <summary>Stops the engine (when running), then builds a new one on the same database and files and starts it. After a
    /// <see cref="Kill"/> it finishes the kill instead of stopping. A stop that hit the 30 s cap fails it (G8 N1: only a
    /// host's exit is safe after that), as does a killed engine that doesn't go quiet within 10 s.</summary>
    public async Task Restart()
    {
        if (_killedAt != null) await FinishKill();
        else
        {
            await Stop();
            if (LastStop >= StopCap) throw new InvalidOperationException($"the stop took {LastStop.TotalSeconds:0.0} s: it hit the 30 s cap");
        }
        Build();
        await Start();
    }

    /// <summary>Kills the engine at a crash point (decision 240): called at the point (on the executor or a worker), it freezes
    /// the files (no write lands from now on), posts JobService.stop ahead of whatever the point unwinds into (so the runner
    /// stores nothing more) and notes the bus's last seq; it returns what the point must throw. The store stays open until
    /// the old executor is quiet (<see cref="Restart"/>): closing a connection in the middle of a call isn't safe.</summary>
    public Exception Kill(string point)
    {
        Fs.Freeze();
        _killedAt = point;
        KilledSeq = Bus.LastSeq;
        var jobs = _jobs!;
        Executor.Post(() => _ = jobs.Stop());
        return new InvalidOperationException("killed at " + point);
    }

    /// <summary>Once the killed engine's executor is quiet (at most 10 s): its executor stops, its store closes, its timers are
    /// dropped and the files thaw.</summary>
    async Task FinishKill()
    {
        var point = _killedAt;
        var deadline = DateTime.UtcNow + KillCap;
        while (Executor.Activity > 0)
        {
            if (DateTime.UtcNow > deadline) throw new InvalidOperationException($"the engine killed at {point} didn't go quiet within 10 s");
            await Task.Delay(5);
        }
        Executor.Stop();
        _store?.Close();
        Clock.CancelAll();
        Fs.Thaw();
        _killedAt = null;
        _stopped = true;
    }

    /// <summary>The library folder's volume goes away (its files are kept aside), and LibraryService checks at once
    /// (owner answer 10: no OS event reports a library's volume). After a kill the volume goes while no engine runs: the files
    /// stay frozen for the killed one, which the kill step has settled, and the next start finds the library offline.</summary>
    public Task Unmount(string path)
    {
        var killed = Fs.Frozen;
        if (killed) Fs.Thaw();
        Fs.Rename(path, Root + "/.unmounted" + path.Replace('/', '_'));
        if (!killed) return Check();
        Fs.Freeze();
        return Task.CompletedTask;
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
            if (_killedAt != null) Task.Run(FinishKill).Wait(TimeSpan.FromSeconds(15));
            else if (!_stopped) Task.Run(Stop).Wait(TimeSpan.FromSeconds(40));
        }
        catch (AggregateException) { }
        _store?.Close();
        try { Directory.Delete(Folder, recursive: true); }
        catch (IOException) { }
    }
}
