using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Engine;

/// <summary>
/// Runs once at startup, before the libraries start (plan §11.1, §15.1, §20.4, §15.8; review L3-3, L3-9). It sorts out
/// the jobs the last engine left unfinished by where each stopped and what its step can do after a stop (its
/// Resumability), rolls open commit intents forward, sets aside staging folders no job will continue, and removes
/// the temporary files a stopped write leaves. Library work runs on a worker per library and may take at most 30 s of
/// the startup: a share that doesn't answer can't keep the engine from starting, and its work goes on.
/// shared/fixtures/engine/recovery-service.cases.json.
/// </summary>
public sealed class RecoveryService
{
    static readonly Duration Patience = new(30);

    readonly EngineExecutor _executor;
    readonly EventBus _bus;
    readonly ProblemService _problems;
    readonly IStore _store;
    readonly StepRegistry _registry;
    readonly LibraryService _library;
    readonly IConfigSource _config;
    readonly EngineClock _clock;
    readonly IFileSystem _fs;
    readonly ISettingsIsolation _isolation;
    readonly IMessageRenderer _renderer;
    readonly string _data;
    readonly object _logLock = new();

    /// <param name="dataDirectory">The engine's data folder: a job's folder is &lt;data&gt;/jobs/&lt;id&gt;.</param>
    public RecoveryService(EngineExecutor executor, EventBus bus, ProblemService problems, IStore store, StepRegistry registry,
        LibraryService library, IConfigSource config, EngineClock clock, IFileSystem fs, ISettingsIsolation isolation, IMessageRenderer renderer,
        string dataDirectory)
    {
        _executor = executor;
        _bus = bus;
        _problems = problems;
        _store = store;
        _registry = registry;
        _library = library;
        _config = config;
        _clock = clock;
        _fs = fs;
        _isolation = isolation;
        _renderer = renderer;
        _data = dataDirectory;
    }

    /// <summary>On the executor. recovery.summary{count} when the last engine left jobs unfinished; null otherwise.</summary>
    public async Task<BroMessage?> Recover()
    {
        var jobs = await _executor.Worker(() => _store.Jobs().Active());
        var homes = jobs.Select(j => JobDirectory(j.Id) + "/home").ToList();
        var found = await _executor.Worker(() => _isolation.Recover(homes));
        foreach (var problem in found)
        {
            var path = problem.Params["path"]?.AsString ?? "";
            if (jobs.FirstOrDefault(j => path.StartsWith(JobDirectory(j.Id) + "/", StringComparison.Ordinal)) is { } owner) await Log(owner.Id, problem);
            else _problems.Raise(problem);
        }

        var unfinished = new HashSet<string>(StringComparer.Ordinal);
        foreach (var job in jobs)
            if (await RecoverJob(job)) unfinished.Add(job.Id.Value);

        // Counted on the executor and completed there: a combinator (Task.WhenAll/WhenAny) would finish on the thread
        // pool, where the executor looks idle for a moment although recover hasn't returned.
        var libraries = Config.Libraries(_config.Current());
        var finished = new TaskCompletionSource();
        var pending = libraries.Count + 1;
        void OneDone()
        {
            if (--pending == 0) finished.TrySetResult();
        }
        async void Track(Task work) // a failure that isn't a BroFailure is a bug, and reaches the executor's handler
        {
            try { await work; }
            finally { OneDone(); }
        }
        var timer = _clock.Timer(new TimerSchedule.At(new Instant(_clock.Now().UnixMilliseconds + (long)(Patience.Seconds * 1000))),
            () => finished.TrySetResult());
        foreach (var l in libraries) Track(Tidy(l.Id, l.Path, unfinished));
        Track(RemoveLeftovers(_data, recursive: true));
        await finished.Task; // what is still out goes on by itself
        timer.Cancel();

        return jobs.Count == 0 ? null : new BroMessage(MessageCode.RecoverySummary, Severity.Warning, ("count", JsonValue.Of(jobs.Count)));
    }

    // ---- jobs (executor) ---------------------------------------------------------------------------------------------

    /// <summary>Requeues, resumes or finishes the job; whether it is still unfinished.</summary>
    async Task<bool> RecoverJob(JobRecord job)
    {
        var rows = await _executor.Worker(() => _store.Steps().Load(job.Id));
        var done = JobPlanner.Done(rows);
        var results = done.Select(s => new StepResult(s.Kind, s.State, s.Error)).ToList();
        var steps = job.Plan?.Steps ?? JobPlanner.Initial(job.Request);
        var row = rows.FirstOrDefault(r => r.Seq == done.Count);

        // The runner stopped between a step that ended (or the last one) and the job's record.
        if (done.Count >= steps.Count || row is { State: StepState.Failed or StepState.Cancelled })
        {
            if (row != null && done.Count < steps.Count) results.Add(new StepResult(row.Kind, row.State, row.Error));
            await Finish(job, JobOutcome.Decide(results, row?.State == StepState.Cancelled, OutcomePolicy.Default), null);
            return false;
        }

        var kind = steps[done.Count];
        var started = row is { State: StepState.Running or StepState.Interrupted };
        var resumability = _registry.Step(kind)?.Resumability ?? Resumability.NotResumable;
        var step = JsonValue.Of(EnumWire.Name(kind));
        if (started && (resumability == Resumability.NotResumable || (resumability == Resumability.ResumeFrom && row!.Checkpoint == null)))
        {
            await SaveStep(row! with { State = StepState.Interrupted });
            results.Add(new StepResult(kind, StepState.Interrupted));
            await Finish(job, JobOutcome.Decide(results, false, OutcomePolicy.Default),
                new BroMessage(MessageCode.RecoveryInterrupted, Severity.Error, ("step", step)));
            return false;
        }
        if (started && resumability == Resumability.ResumeFrom)
        {
            await Requeue(job, job.Queue, new BroMessage(MessageCode.RecoveryResumed, Severity.Info, ("step", step)));
            return true;
        }
        if (done.Any(s => s.Kind == StepKind.Acquire))
        {
            var queue = done.Any(s => s.Kind == StepKind.Release) ? Queue.Processing : job.Queue;
            await Requeue(job, queue, new BroMessage(MessageCode.RecoveryResumed, Severity.Info, ("step", step)));
            return true;
        }

        // Before Acquire. A media generation doesn't outlive the engine (owner, 2026-10-14): an automatic job can't know
        // its disc is still there (the new run's insert starts a fresh job), and a manual one reads the drive again.
        if (job.Request.DriveId != null && job.Request.Automatic)
        {
            if (started) await SaveStep(row! with { State = StepState.Interrupted });
            var error = new BroError(MessageCode.Wire(MessageCode.DiscChanged), ("why", new BroMessage(MessageCode.DiscChangedGeneration).ToJson()));
            await Finish(job, new Decided(Outcome.Cancelled, error), new BroMessage(MessageCode.DiscChanged, error.Params, Severity.Error));
            return false;
        }
        if (job.Request.DriveId != null)
        {
            foreach (var r in rows)
                await SaveStep(r with { State = StepState.Pending, StartedAt = null, FinishedAt = null, Output = null, Error = null, Checkpoint = null });
            job = job with { Plan = null, MediaGeneration = null, Request = job.Request with { MediaGeneration = null } };
        }
        await Requeue(job, job.Queue, new BroMessage(MessageCode.RecoveryRequeued));
        return true;
    }

    async Task Requeue(JobRecord job, Queue queue, BroMessage line)
    {
        var record = job with { State = JobState.Queued, Queue = queue, Decision = null, BlockedBy = null };
        await _executor.Worker(() => _store.Jobs().Update(record));
        await Log(job.Id, line);
        _bus.Publish(EngineEventType.JobChanged, JsonValue.Of(("job", JobDto.Json(JobDto.Of(record)))));
    }

    async Task Finish(JobRecord job, Decided decided, BroMessage? line)
    {
        var record = job with { State = JobState.Finished, Outcome = decided.Outcome, Error = decided.Error, Decision = null, BlockedBy = null,
            FinishedAt = _clock.Now() };
        await _executor.Worker(() => _store.Jobs().Update(record));
        if (line != null) await Log(job.Id, line);
        _bus.Publish(EngineEventType.JobFinished, JsonValue.Of(("job", JobDto.Json(JobDto.Of(record)))));
    }

    Task SaveStep(StepRecord step) => _executor.Worker(() => _store.Steps().Save(step));

    /// <summary>Appends the line to the job's log.jsonl, on a worker (one append at a time: libraries are tidied side by
    /// side).</summary>
    Task Log(Id jobId, BroMessage message)
    {
        var line = new LogLineDto(0, _clock.Now(), message.Severity, message.Code, message.Params);
        return _executor.Worker(() =>
        {
            lock (_logLock) JobLog.Append(_fs, JobDirectory(jobId), new[] { line });
            return true;
        });
    }

    // ---- libraries and files -------------------------------------------------------------------------------

    /// <summary>One library: its open intents rolled forward, then its staging folders tidied and the ones no job will
    /// continue set aside. An offline library is left alone (I2). On the executor; the file and store work on workers.</summary>
    async Task Tidy(string libraryId, string root, IReadOnlySet<string> unfinished)
    {
        foreach (var intent in await _executor.Worker(() => _store.Units().OpenIntents()))
        {
            var unit = await _executor.Worker(() => _store.Units().Get(intent.UnitId));
            if (unit?.LibraryId != libraryId) continue;
            try
            {
                await _library.RollForward(intent);
                await Log(intent.JobId, new BroMessage(MessageCode.CommitRolledForward, Severity.Warning, ("unit", JsonValue.Of(intent.UnitId.Value))));
                await RemoveLeftovers(intent.Destination, recursive: false);
            }
            catch (BroFailure f)
            {
                // The intent stays: the job's Commit step rolls it forward when it runs again.
                await Log(intent.JobId, new BroMessage(MessageCode.Parse(f.Error.Code) ?? MessageCode.InternalUnexpected, f.Error.Params, Severity.Warning));
            }
        }
        var committing = (await _executor.Worker(() => _store.Units().OpenIntents())).Select(i => i.Staging).ToHashSet(StringComparer.Ordinal);
        IReadOnlyList<string> folders;
        try { folders = await _library.StagingFolders(libraryId); }
        catch (BroFailure) { return; }
        foreach (var folder in folders)
        {
            var path = root + "/.bromelia/staging/" + folder;
            await RemoveLeftovers(path, recursive: true);
            if (unfinished.Contains(folder) || committing.Contains(path)) continue;
            var job = await _executor.Worker(() => _store.Jobs().Load(new Id(folder)));
            if (job?.Outcome == Outcome.Interrupted) continue; // kept so it can be continued (owner, 2026-10-03)
            var lines = Notes.Render(new Id(folder), job?.Outcome ?? Outcome.Interrupted, job?.Error, Array.Empty<RobotMessage>(), Array.Empty<string>());
            var note = Encoding.UTF8.GetBytes(string.Concat(lines.Select(l => _renderer.Render(l) + "\n")));
            var name = Sanitizer.Component(job?.Title ?? "") is { Length: > 0 } title ? title : folder;
            try { await _library.SetAside(libraryId, folder, name, note); }
            catch (BroFailure) { } // left hidden in staging; the next start tries again
        }
    }

    Task RemoveLeftovers(string folder, bool recursive) => _executor.Worker(() =>
    {
        RemoveLeftoversNow(folder, recursive);
        return true;
    });

    /// <summary>Removes what a stopped write leaves (ArchiveFiles.isLeftover) from the folder, and its subfolders when
    /// <paramref name="recursive"/>.</summary>
    void RemoveLeftoversNow(string folder, bool recursive)
    {
        try
        {
            if (!_fs.Exists(folder)) return;
            foreach (var e in _fs.List(folder).OrderBy(e => e.Name, StringComparer.Ordinal))
            {
                var path = folder + "/" + e.Name;
                if (e.IsDirectory)
                {
                    if (recursive) RemoveLeftoversNow(path, true);
                }
                else if (ArchiveFiles.IsLeftover(e.Name))
                {
                    try { _fs.Remove(path); }
                    catch (BroFailure) { }
                }
            }
        }
        catch (BroFailure) { }
    }

    string JobDirectory(Id jobId) => _data + "/jobs/" + jobId.Value;
}
