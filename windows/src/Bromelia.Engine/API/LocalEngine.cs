using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Engine;

/// <summary>
/// Implements <see cref="IEngineAPI"/> over the services (plan §13.1, §15.1). Callable from any thread (never from the
/// executor's): each call hops onto the EngineExecutor. The composition root builds it, then the host calls
/// <see cref="Start"/> and calls nothing else before that completes. After <see cref="Stop"/> every call answers
/// server.stopping. shared/fixtures/engine/local-engine.cases.json.
/// </summary>
public sealed class LocalEngine : IEngineAPI
{
    /// <summary>How long stop waits for the jobs to settle and the executor to go idle; the host exits anyway after it.</summary>
    static readonly TimeSpan SettleCap = TimeSpan.FromSeconds(30);
    /// <summary>The finished jobs a snapshot carries, newest first.</summary>
    const int FinishedInSnapshot = 50;

    readonly EngineExecutor _executor;
    readonly EventBus _bus;
    readonly EngineClock _clock;
    readonly ProblemService _problems;
    readonly ConfigService _config;
    readonly Scheduler _scheduler;
    readonly LibraryService _libraries;
    readonly DriveService _drives;
    readonly JobService _jobs;
    readonly RecoveryService _recovery;
    readonly IToolLocator _locator;
    readonly IEngineLog _log;
    readonly EngineFacts _facts;
    readonly Action _release;
    readonly List<EventFeed> _feeds = new();
    Subscription? _limits;
    Instant _startedAt;
    bool _stopping;

    /// <param name="release">Called once stop has stopped the executor: the root closes the store there.</param>
    public LocalEngine(EngineExecutor executor, EventBus bus, EngineClock clock, ProblemService problems, ConfigService config,
        Scheduler scheduler, LibraryService libraries, DriveService drives, JobService jobs, RecoveryService recovery, IToolLocator locator,
        IEngineLog log, EngineFacts facts, Action release)
    {
        _executor = executor;
        _bus = bus;
        _clock = clock;
        _problems = problems;
        _config = config;
        _scheduler = scheduler;
        _libraries = libraries;
        _drives = drives;
        _jobs = jobs;
        _recovery = recovery;
        _locator = locator;
        _log = log;
        _facts = facts;
        _release = release;
    }

    // ---- start and stop ----

    /// <summary>The startup order (plan §15.1, decision 169): the configuration (a failed load fails start, and nothing
    /// else has started), recovery (recovery.summary into the engine log), the configuration's limits to the Scheduler
    /// (and on every configChanged), the libraries, the drives, then the jobs recovery left queued.</summary>
    public Task Start() => On(async () =>
    {
        _startedAt = _clock.Now();
        await _config.Load();
        if (await _recovery.Recover() is { } summary) _log.Log(summary);
        ApplyLimits();
        _limits = _bus.Subscribe(new[] { EngineEventType.ConfigChanged }, _ => ApplyLimits());
        await _libraries.Start();
        await _drives.Start();
        await _jobs.Start();
        return true;
    });

    /// <summary>The stop order (decision 170): from now on every call answers server.stopping; the feeds close and the
    /// intake stops (drives, libraries); the runner shuts every live job down storing nothing more, so the store is what
    /// a killed engine leaves; then, once no job is live and the executor is idle (at most 30 s), the executor stops and
    /// the root's release runs.</summary>
    public async Task Stop()
    {
        var shutdown = await On(() =>
        {
            _stopping = true;
            foreach (var feed in _feeds) feed.Close();
            _feeds.Clear();
            _limits?.Cancel();
            _limits = null;
            _drives.Stop();
            _libraries.Stop();
            return Task.FromResult(_jobs.Stop());
        });
        var cap = Task.Delay(SettleCap);
        await Task.WhenAny(shutdown, cap).ConfigureAwait(false);
        while (_executor.Activity > 0 && !cap.IsCompleted) await Task.Delay(5).ConfigureAwait(false);
        _executor.Stop();
        _release();
    }

    /// <summary>A phase 3 door outside EngineAPI (G1 owner question 5): a job for the request (an ISO or a folder needs
    /// SessionService otherwise), for the scenario runner and the real-ISO test. Retired when openSession/ripSession come.</summary>
    public Task<JobDto> StartJob(JobRequest request) => On(() => _jobs.Create(request));

    // ---- system ----

    public Task<SystemStatusDto> System() => On(Status);

    // ---- drives ----

    public Task<IReadOnlyList<DriveDto>> Drives() => On(() => Task.FromResult(_drives.Drives()));

    public Task Eject(string driveId) => On(async () =>
    {
        await _drives.Eject(driveId);
        return true;
    });

    public Task CloseTray(string driveId) => On(async () =>
    {
        await _drives.CloseTray(driveId);
        return true;
    });

    public Task CloseAllTrays() => On(async () =>
    {
        await _drives.CloseAll();
        return true;
    });

    public Task<JobDto> Rip(string driveId, RipOptionsDto options) => On(async () =>
    {
        var drive = _drives.Drives().FirstOrDefault(d => d.Id == driveId && d.Connected)
            ?? throw Failure(MessageCode.DriveNotFound, ("driveId", JsonValue.Of(driveId)));
        if (drive.Medium is not { } medium) throw Failure(MessageCode.DriveNoDisc);
        if (drive.Busy) throw Failure(MessageCode.DriveBusy);
        if (medium.Content is DiscContent.Audio or DiscContent.Data) throw Failure(MessageCode.DiscNotVideo, ("drive", JsonValue.Of(drive.Name)));
        if (options.Titles != null) throw Failure(MessageCode.SessionNotOpen);
        var library = options.Library;
        if (options.OutputFolder is { } folder)
            library = _libraries.Libraries().FirstOrDefault(l => SamePath(l.Path, folder))?.Id
                ?? throw Failure(MessageCode.LibraryNotAttached, ("path", JsonValue.Of(folder)));
        var chosen = new List<(string, JsonValue)>();
        if (options.Mode is { } mode) chosen.Add(("mode", JsonValue.Of(EnumWire.Name(mode))));
        if (options.Profile is { } profile) chosen.Add(("profile", JsonValue.Of(profile)));
        if (library != null) chosen.Add(("library", JsonValue.Of(library)));
        var title = medium.Label.Length > 0 ? medium.Label : drive.Name;
        return await _jobs.Create(new JobRequest(JobKind.VideoDisc, title, false, drive.Id, medium.Generation, Options: JsonValue.Of(chosen.ToArray())));
    });

    // ---- jobs ----

    public Task<JobPageDto> Jobs(JobQueryDto query) => On(() => _jobs.Query(query));

    public Task<JobDetailDto> Job(Id jobId) => On(() => _jobs.Job(jobId));

    public Task<IReadOnlyList<LogLineDto>> JobLog(Id jobId, long after) => On(() => _jobs.Log(jobId, after));

    public Task Cancel(Id jobId) => On(async () =>
    {
        await _jobs.Cancel(jobId);
        return true;
    });

    public Task<JobDto> Retry(Id jobId, StepKind? fromStep) => On(() => _jobs.Retry(jobId, fromStep));

    public Task KeepApart(Id jobId) => On(() => _jobs.KeepApart(jobId));

    public Task Decide(Id jobId, string decisionId, string answer) => On(async () =>
    {
        await _jobs.Decide(jobId, decisionId, answer);
        return true;
    });

    public Task Move(Id jobId, int by) => On(async () =>
    {
        await _jobs.Move(jobId, by);
        return true;
    });

    public Task StartNow(Id jobId) => On(async () =>
    {
        await _jobs.StartNow(jobId);
        return true;
    });

    // ---- libraries ----

    public Task<IReadOnlyList<LibraryDto>> Libraries() => On(() => Task.FromResult(_libraries.Libraries()));

    /// <summary>The id is the name (else the folder's last part) made into an EntryId, with -2, -3 … when the
    /// configuration has it already (decision 187). LibraryService writes the marker, then the configuration gets
    /// {id, name, path} at its current version; if that save fails the marker is removed and the save's error
    /// returned, so a retry doesn't meet library.alreadyAttached.</summary>
    public Task<LibraryDto> AttachLibrary(string path, string name) => On(async () =>
    {
        var current = _config.Current();
        var display = name.Length > 0 ? name : LastPart(path);
        var id = NewLibraryId(display, Domain.Config.Libraries(current).Select(l => l.Id).ToList());
        var attached = await _libraries.Attach(id, path, name);
        var entry = JsonValue.Of(("id", JsonValue.Of(id)), ("name", JsonValue.Of(attached.Name)), ("path", JsonValue.Of(path)));
        var libraries = new List<JsonValue>(current.Document["libraries"]?.AsArray ?? new List<JsonValue>()) { entry };
        var members = ((JsonValue.Object)current.Document).Members.Where(m => m.Key != "libraries").ToList();
        members.Add(new("libraries", JsonValue.Of(libraries)));
        try
        {
            await _config.Update(new JsonValue.Object(members), _config.Version());
        }
        catch (BroFailure)
        {
            await _libraries.RemoveMarker(id);
            throw;
        }
        return _libraries.Libraries().FirstOrDefault(l => l.Id == id) ?? attached;
    });

    // ---- configuration ----

    public Task<ConfigEnvelope> Config() => On(() => _config.Envelope());

    public Task<ConfigEnvelope> UpdateConfig(JsonValue doc, long version) => On(() => _config.Update(doc, version));

    public Task<IReadOnlyList<Issue>> ValidateConfig(JsonValue doc) => On(() => Task.FromResult(_config.Validate(doc)));

    public Task SetSecret(string name, string value) => On(async () =>
    {
        await _config.SetSecret(name, value);
        return true;
    });

    public Task<JsonValue> ExportConfig() => On(() => Task.FromResult(JsonValue.Parse(_config.Export())!));

    // ---- events ----

    /// <summary>Decision 168, 186: with <paramref name="after"/> the feed replays from it (one the bus can't replay
    /// starts with a resync, which the feed answers with a snapshot); without it the first event is a snapshot whose seq
    /// is the last event's before the snapshot was gathered, and the events published while it was gathered follow it as
    /// deltas.</summary>
    public Task<EventFeed> Events(long? after, IReadOnlyCollection<Id> jobLog) => On(async () =>
    {
        EventStream stream;
        EngineEvent? first = null;
        if (after is { } a)
            stream = _bus.Stream(a, null);
        else
        {
            first = await Snapshot();
            if (_stopping) throw Failure(MessageCode.ServerStopping);
            stream = _bus.Stream(first.Seq, null);
        }
        var feed = new EventFeed(stream, jobLog, first, () => On(Snapshot));
        _feeds.RemoveAll(f => f.IsClosed);
        _feeds.Add(feed);
        return feed;
    });

    // ---- inside ----

    /// <summary>Runs <paramref name="work"/> on the executor; its result or failure comes back to the caller.
    /// server.stopping once stop has begun.</summary>
    Task<T> On<T>(Func<Task<T>> work)
    {
        var done = new TaskCompletionSource<T>(TaskCreationOptions.RunContinuationsAsynchronously);
        if (!_executor.Post(() => _ = Run(work, done))) done.SetException(Failure(MessageCode.ServerStopping));
        return done.Task;
    }

    async Task Run<T>(Func<Task<T>> work, TaskCompletionSource<T> done)
    {
        try
        {
            if (_stopping) throw Failure(MessageCode.ServerStopping);
            done.SetResult(await work());
        }
        catch (Exception e)
        {
            done.SetException(e);
        }
    }

    void ApplyLimits() => _scheduler.UpdateLimits(SchedulerLimits.Of(_config.Current(), _facts.Cores));

    async Task<SystemStatusDto> Status()
    {
        var tools = await _executor.Worker(() => new[] { _locator.Locate(ToolKind.Makemkvcon), _locator.Locate(ToolKind.Mkvmerge) });
        var makemkv = tools[0];
        return new SystemStatusDto(_facts, _startedAt, makemkv.Path != null, makemkv.Version, tools, _problems.Problems(), _libraries.Libraries(),
            _config.Version(), _bus.LastSeq);
    }

    /// <summary>The snapshot event (never published): its seq is the last event's when gathering began.</summary>
    async Task<EngineEvent> Snapshot()
    {
        long seq = _bus.LastSeq;
        var unfinished = new List<JobDto>();
        var states = new[] { JobState.Queued, JobState.WaitingForResources, JobState.Running, JobState.Blocked, JobState.AwaitingDecision };
        string? cursor = null;
        do
        {
            var page = await _jobs.Query(new JobQueryDto(states, Array.Empty<Outcome>(), Cursor: cursor, Limit: JobQueryDto.MaxLimit));
            unfinished.AddRange(page.Items);
            cursor = page.Next;
        }
        while (cursor != null);
        var finished = await _jobs.Query(new JobQueryDto(new[] { JobState.Finished }, Array.Empty<Outcome>(), Limit: FinishedInSnapshot));
        var system = await Status();
        var snapshot = new SnapshotDto(system with { EventSeq = seq }, _drives.Drives(), unfinished.Concat(finished.Items).ToList(),
            _scheduler.Queues(), _problems.Problems(), _libraries.Libraries(), _config.Version());
        return new EngineEvent(seq, _clock.Now(), EngineEventType.Snapshot, SnapshotDto.Json(snapshot));
    }

    static BroFailure Failure(MessageCode code, params (string Key, JsonValue Value)[] parameters) =>
        new(new BroError(MessageCode.Wire(code), parameters));

    static bool SamePath(string a, string b) => a.TrimEnd('/', '\\') == b.TrimEnd('/', '\\');

    static string LastPart(string path)
    {
        var trimmed = path.TrimEnd('/', '\\');
        int i = trimmed.LastIndexOfAny(new[] { '/', '\\' });
        return i >= 0 ? trimmed[(i + 1)..] : trimmed;
    }

    /// <summary>The name as an EntryId (common.json: letters, digits, '.', '_', '-'; at most 64): ASCII letters and
    /// digits lowercased, every run of anything else one '-', no '-' at either end, at most 60 characters; "library"
    /// when nothing is left. Then -2, -3 … until no id of <paramref name="taken"/> has it (ignoring case).</summary>
    static string NewLibraryId(string name, IReadOnlyCollection<string> taken)
    {
        var id = new StringBuilder();
        foreach (char c in name)
        {
            if (c is >= 'a' and <= 'z' or >= '0' and <= '9') id.Append(c);
            else if (c is >= 'A' and <= 'Z') id.Append((char)(c + 32));
            else if (id.Length > 0 && id[^1] != '-') id.Append('-');
        }
        var stem = id.ToString().TrimEnd('-');
        if (stem.Length > 60) stem = stem[..60].TrimEnd('-');
        if (stem.Length == 0) stem = "library";
        var lower = taken.Select(t => t.ToLowerInvariant()).ToHashSet(); // EntryIds are ASCII
        var candidate = stem;
        for (int n = 2; lower.Contains(candidate); n++) candidate = stem + "-" + n.ToString(CultureInfo.InvariantCulture);
        return candidate;
    }
}
