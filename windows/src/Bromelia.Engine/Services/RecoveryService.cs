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
/// Runs once at startup, before the libraries start (plan §11.1, §15.1, §20.4, §15.8). It sorts out the jobs the last
/// engine left unfinished by where each stopped and what its step can do after a stop (its Resumability), rolls open
/// commit intents forward, sets aside staging folders no job will continue (a failed or cancelled job's with its logs),
/// and removes the temporary files a stopped write leaves. Library work runs on a worker per library and may take at
/// most 30 s of the startup: a share that doesn't answer can't keep the engine from starting, and its work goes on, but
/// no longer on what a live job may be using. shared/fixtures/engine/recovery-service.cases.json.
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

    /// <summary>Recover stopped waiting for its libraries, so jobs may be live.</summary>
    volatile bool _late;

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

    /// <summary>On the executor. recovery.summary{count} when the last engine left jobs unfinished; null
    /// otherwise.</summary>
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
        async void Track(Task work) // Tidy and RemoveLeftovers catch their own BroFailures; anything else is a bug and reaches the executor's handler
        {
            try { await work; }
            finally { OneDone(); }
        }
        var timer = _clock.Timer(new TimerSchedule.At(new Instant(_clock.Now().UnixMilliseconds + (long)(Patience.Seconds * 1000))),
            () =>
            {
                _late = true;
                finished.TrySetResult();
            });
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
        var results = done.Select(s => new StepResult(s.Kind, s.State, s.Error, s.Output?.ReadErrors ?? 0)).ToList();
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
        var registered = _registry.Step(kind);
        var resumability = registered?.Resumability ?? Resumability.NotResumable;
        var step = JsonValue.Of(EnumWire.Name(kind));
        bool resumes = resumability == Resumability.ResumeFrom && registered!.ResumeCheckpoint is { } wanted
            && row?.Checkpoint?["kind"]?.AsString == wanted;
        if (started && (resumability == Resumability.NotResumable || (resumability == Resumability.ResumeFrom && !resumes)))
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
        // Past Acquire, or keeping an interrupted job's files apart (which needs no drive): resumed.
        if (done.Any(s => s.Kind == StepKind.Acquire) || kind == StepKind.KeepApart)
        {
            var queue = done.Any(s => s.Kind == StepKind.Release) || kind == StepKind.KeepApart ? Queue.Processing : job.Queue;
            await Requeue(job, queue, new BroMessage(MessageCode.RecoveryResumed, Severity.Info, ("step", step)));
            return true;
        }

        // Before Acquire. A media generation doesn't outlive the engine: an automatic job can't know its disc is still
        // there (the new run's insert starts a fresh job), and a manual one reads the drive again.
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

    /// <summary>A job that finished failed in its Commit or KeepApart step, or cancelled with its Commit step
    /// unfinished, whose commit a roll forward has just finished, gets the outcome that step gives when it succeeds:
    /// JobOutcome.decide of its steps with that step succeeded, so succeeded or succeededWithReadErrors after a Commit,
    /// cancelled after a KeepApart. That step is the first row not done. In a cancelled job a Commit row counts in any
    /// state, as however it got there (plan §20.4) is the rule: cancelled or failed by a cancel in the step, pending
    /// after a retry, running after a crash. In a failed job the row must be failed or cancelled: the step ended there,
    /// and a pending or running one keeps its outcome. Its finishedAt, step rows and log stay as they were.</summary>
    async Task Committed(Id jobId)
    {
        var job = await _executor.Worker(() => _store.Jobs().Load(jobId));
        if (job is not { State: JobState.Finished, Outcome: Outcome.Failed or Outcome.Cancelled }) return;
        var rows = await _executor.Worker(() => _store.Steps().Load(jobId));
        var done = JobPlanner.Done(rows);
        if (rows.FirstOrDefault(r => r.Seq == done.Count) is not { } stopped) return;
        var ended = stopped.State is StepState.Failed or StepState.Cancelled && stopped.Kind is StepKind.Commit or StepKind.KeepApart;
        if (!(job.Outcome == Outcome.Cancelled ? stopped.Kind == StepKind.Commit : ended)) return;
        var results = done.Select(s => new StepResult(s.Kind, s.State, s.Error, s.Output?.ReadErrors ?? 0))
            .Append(new StepResult(stopped.Kind, StepState.Succeeded)).ToList();
        var decided = JobOutcome.Decide(results, false, OutcomePolicy.Default);
        var record = job with { Outcome = decided.Outcome, Error = decided.Error };
        await _executor.Worker(() => _store.Jobs().Update(record));
        _bus.Publish(EngineEventType.JobFinished, JsonValue.Of(("job", JobDto.Json(JobDto.Of(record)))));
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

    /// <summary>For a line about work that is already done or kept: the line can be lost, the work's result can't.</summary>
    async Task LogIfPossible(Id jobId, BroMessage message)
    {
        try { await Log(jobId, message); }
        catch (BroFailure) { }
    }

    // ---- libraries and files -------------------------------------------------------------------------------

    /// <summary>One library: its open intents rolled forward (a job that failed in that commit, or was cancelled with
    /// it unfinished, then gets the outcome it gives), then its staging folders tidied and the ones no job will
    /// continue set aside. An offline library is left alone, and so is the staging of one whose store reads fail: a
    /// read that fails can't say what is open or whose a folder is, and the next start tries again. On the executor;
    /// the file and store work on workers.</summary>
    async Task Tidy(string libraryId, string root, IReadOnlySet<string> unfinished)
    {
        try
        {
            await RollForwards(libraryId);
            await SetAsides(libraryId, root, unfinished);
        }
        catch (BroFailure) { }
    }

    async Task RollForwards(string libraryId)
    {
        foreach (var intent in await _executor.Worker(() => _store.Units().OpenIntents()))
        {
            var unit = await _executor.Worker(() => _store.Units().Get(intent.UnitId));
            if (unit?.LibraryId != libraryId) continue;
            try
            {
                if (await _library.RollForward(intent) == null) continue; // the job's Commit finished it meanwhile
                await LogIfPossible(intent.JobId, new BroMessage(MessageCode.CommitRolledForward, Severity.Warning, ("unit", JsonValue.Of(intent.UnitId.Value))));
                await Committed(intent.JobId);
                await RemoveLeftovers(intent.Destination, recursive: false);
            }
            catch (BroFailure f)
            {
                // A roll forward that failed keeps its intent: the job's Commit step rolls it forward when it runs
                // again. A job whose new outcome couldn't be stored keeps its old one with this line in its log; a
                // retry from Commit finds the unit committed.
                await LogIfPossible(intent.JobId, new BroMessage(MessageCode.Parse(f.Error.Code) ?? MessageCode.InternalUnexpected, f.Error.Params, Severity.Warning));
            }
        }
    }

    async Task SetAsides(string libraryId, string root, IReadOnlySet<string> unfinished)
    {
        // By job, not by staging path: an intent's paths are absolute, and the library's root may have moved since.
        var committing = (await _executor.Worker(() => _store.Units().OpenIntents())).Select(i => i.JobId.Value).ToHashSet(StringComparer.Ordinal);
        foreach (var folder in await _library.StagingFolders(libraryId))
        {
            var path = root + "/.bromelia/staging/" + folder;
            // Read after the listing, and a job's row is stored before its folder is made: no row means no job.
            var job = await _executor.Worker(() => _store.Jobs().Load(new Id(folder)));
            var rows = job == null ? Array.Empty<StepRecord>() : await _executor.Worker(() => _store.Steps().Load(job.Id));
            await RemoveLeftovers(path, recursive: true);
            if (unfinished.Contains(folder) || committing.Contains(folder)) continue;
            if (job?.Outcome == Outcome.Interrupted) continue; // kept so it can be continued
            var copies = job == null ? new List<(string, byte[])>() : await Logs(job, rows);
            var lines = Notes.Render(new Id(folder), job?.Outcome ?? Outcome.Interrupted, job?.Error, Array.Empty<RobotMessage>(),
                copies.Select(c => c.Item1).ToList(), job == null ? null : JobDirectory(job.Id) + "/" + JobLog.FileName);
            var note = Encoding.UTF8.GetBytes(string.Concat(lines.Select(l => _renderer.Render(l) + "\n")));
            var name = job == null ? folder : KeepApartStep.FolderName(new JobSnapshot(job.Id, job.Request, job.Plan,
                rows.Where(r => r.State == StepState.Succeeded && r.Output != null).OrderBy(r => r.Seq).Select(r => r.Output!).ToList()));
            if (job != null && _late) continue; // live, or live after a retry at any moment; the next start sees
            try { await _library.SetAside(libraryId, folder, name, note, copies); }
            catch (BroFailure) { } // left hidden in staging; the next start tries again
        }
    }

    /// <summary>The job's logs as Seal copies them (SealStep.copyLogs: its log as text, MakeMKV's transcripts, each
    /// when the job folder has it; none when its Plan's profile, in <paramref name="rows"/>, has archive.archiveRecord
    /// false), by name; on a worker. When they can't be read, the files are set aside without them.</summary>
    Task<List<(string, byte[])>> Logs(JobRecord job, IReadOnlyList<StepRecord> rows) => _executor.Worker(() =>
    {
        var copies = new List<(string, byte[])>();
        try
        {
            var plan = rows.LastOrDefault(r => r.Kind == StepKind.Plan && r.State == StepState.Succeeded)?.Output
                is { Data: JsonValue.Object data } ? PlanOutput.Decode(data) : null;
            bool copy = plan?.Profile["archive"]?["archiveRecord"]?.AsBool ?? true;
            SealStep.CopyLogs(_fs, JobDirectory(job.Id), _renderer, ArchiveFiles.UnitId(job.Id).Value[..8], copy, (name, bytes) => copies.Add((name, bytes)));
        }
        catch (BroFailure)
        {
            copies.Clear();
        }
        return copies;
    });

    Task RemoveLeftovers(string folder, bool recursive) => _executor.Worker(() =>
    {
        RemoveLeftoversNow(folder, recursive);
        return true;
    });

    /// <summary>Removes what a stopped write leaves (ArchiveFiles.isLeftover) from the folder, and its subfolders when
    /// <paramref name="recursive"/>. A listing that answers once jobs may be live can't tell that from what they are
    /// writing, so nothing it gives goes; one that answered before that can only name leftovers.</summary>
    void RemoveLeftoversNow(string folder, bool recursive)
    {
        try
        {
            if (!_fs.Exists(folder)) return;
            var entries = _fs.List(folder);
            if (_late) return;
            foreach (var e in entries.OrderBy(e => e.Name, StringComparer.Ordinal))
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
