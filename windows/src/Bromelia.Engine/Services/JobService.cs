using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Engine;

/// <summary>
/// The jobs (plan §11.1, §13.1): it creates them, runs them through the PipelineRunner and keeps the in-memory view of
/// the ones that haven't finished; the Scheduler decides when each step runs. On the executor; the store and the log
/// files are read and written on workers. shared/fixtures/engine/job-service.cases.json.
/// <list type="bullet">
/// <item>A created job is in its queue at once, in the order the jobs were created, and in the store before jobChanged
/// (queued) announces it; then it runs. <see cref="Start"/> runs the jobs the last engine left unfinished (after
/// RecoveryService.recover), in their queues' order.</item>
/// <item>Each job's place in its queue is stored, so the order survives a restart (written in a transaction that
/// changes only the position: the runner owns the rest of the record).</item>
/// <item>The continue offer comes with AutomationService (owner, 2026-10-17/18).</item>
/// </list>
/// </summary>
public sealed class JobService
{
    /// <summary>A job this service runs, until its run ends.</summary>
    sealed class Active
    {
        public bool CancelRequested;
    }

    readonly EngineExecutor _executor;
    readonly EventBus _bus;
    readonly IStore _store;
    readonly Scheduler _scheduler;
    readonly PipelineRunner _runner;
    readonly EngineClock _clock;
    readonly IIdGenerator _ids;
    readonly IFileSystem _fs;
    readonly string _data;
    readonly Dictionary<Id, Active> _active = new();
    /// <summary>Finished jobs a retry or keep-apart is starting again (between reading and storing their rows).</summary>
    readonly HashSet<Id> _restarting = new();
    /// <summary>Where each active job should be stored in its queue; read by the store's workers, hence the lock.</summary>
    readonly Dictionary<Id, int> _positions = new();
    /// <summary>The positions last asked of the store, on the executor.</summary>
    readonly Dictionary<Id, int> _asked = new();
    Subscription? _queues;

    /// <param name="dataDirectory">The engine's data folder: a job's log is &lt;data&gt;/jobs/&lt;id&gt;/log.jsonl.</param>
    public JobService(EngineExecutor executor, EventBus bus, IStore store, Scheduler scheduler, PipelineRunner runner, EngineClock clock,
        IIdGenerator ids, IFileSystem fs, string dataDirectory)
    {
        _executor = executor;
        _bus = bus;
        _store = store;
        _scheduler = scheduler;
        _runner = runner;
        _clock = clock;
        _ids = ids;
        _fs = fs;
        _data = dataDirectory;
    }

    // ---- commands ----

    /// <summary>Runs the jobs the store has unfinished (what RecoveryService left queued), in their queues' order. Call it
    /// once, after recover().</summary>
    public async Task Start()
    {
        _queues ??= _bus.Subscribe(new[] { EngineEventType.QueueChanged }, _ => OnQueueChanged());
        foreach (var job in await _executor.Worker(() => _store.Jobs().Active()))
        {
            if (_active.ContainsKey(job.Id)) continue;
            Admit(job.Id, job.Queue);
            _asked[job.Id] = job.Position; // what the store has: OnQueueChanged writes the place it has now
            await Launch(job);
        }
        OnQueueChanged();
    }

    /// <summary>A new job for the request: queued, stored, announced (jobChanged), then run.</summary>
    public async Task<JobDto> Create(JobRequest request)
    {
        var id = _ids.Next();
        var queue = JobPlanner.Queue(request.Kind);
        var steps = JobPlanner.Initial(request);
        var position = Admit(id, queue);
        var record = new JobRecord(id, request.Kind, request.ParentId, JobState.Queued, null, null, null, null, queue, position, request.DriveId,
            request.MediaGeneration, request.Automatic, null, request.Title, null, request, steps.Count > 0 ? new JobPlan(steps) : null, request.UnitId,
            _clock.Now());
        try
        {
            await _executor.Worker(() => _store.Jobs().Insert(record));
        }
        catch (BroFailure)
        {
            Forget(id);
            _scheduler.Finish(id);
            throw;
        }
        if (_asked[id] != position) SavePosition(id); // the queue moved while the record was written
        var dto = JobDto.Of(record) with { Position = _scheduler.Position(id) };
        _bus.Publish(EngineEventType.JobChanged, JsonValue.Of(("job", JobDto.Json(dto))));
        await Launch(record);
        return dto;
    }

    /// <summary>Cancels the job: a waiting or running one ends cancelled (job.cancelledByUser); one nothing runs is finished
    /// as cancelled at once; a finished one stays as it is. job.notFound when there is none.</summary>
    public async Task Cancel(Id jobId)
    {
        if (_active.TryGetValue(jobId, out var active))
        {
            active.CancelRequested = true;
            _runner.Cancel(jobId);
            return;
        }
        var record = await Load(jobId);
        if (record.State == JobState.Finished || _active.ContainsKey(jobId)) return;
        await FinishCancelled(record);
    }

    /// <summary>Runs a finished job again (plan §13.1's retry(id, fromStep?); owner 2026-10-17/18).
    /// <list type="bullet">
    /// <item>Without a step: one that failed, was cancelled or was skipped, as a new job with the same request, started by
    /// hand (not automatic, no media generation); an interrupted one is continued: the same job from its first step.</item>
    /// <item>From a step: one that failed, was cancelled or was interrupted, the same job, from that step
    /// (job.unknownStep {step} when its plan has none; server.badRequest when a step before it isn't done). A step up to
    /// Release starts it from its first step instead: the disc is read again (Acquire then keeps what an earlier attempt
    /// finished of the same disc).</item>
    /// </list>
    /// The same job's steps from there are stored pending (their checkpoints kept), its plan cut back after Plan when Plan
    /// runs again, and its record queued again (no outcome, by hand, no media generation; acquisition up to Release, else
    /// processing) with job.retried {step} in its log, then it runs like a new one. job.notFound; job.notRetryable for any
    /// other job (read errors and failed post steps get retries of their own). While the job has an open commit intent
    /// (its files are partly in the library), only a retry from Commit is taken, which finishes it: any other is
    /// job.commitOpen (a step before Commit would change staging under the intent; a new job would archive the disc again
    /// beside it). A job that was kept apart is retried from its keepApart step only (that step again, in the processing
    /// queue), or as a new job; any other step is job.notRetryable.</summary>
    public async Task<JobDto> Retry(Id jobId, StepKind? fromStep = null)
    {
        var record = await Load(jobId);
        if (record.State != JobState.Finished || _active.ContainsKey(jobId)) throw NotRetryable(jobId);
        if (fromStep == null && record.Outcome is Outcome.Failed or Outcome.Cancelled or Outcome.Skipped)
        {
            await NotCommitting(jobId);
            return await Create(record.Request with { Automatic = false, MediaGeneration = null });
        }
        if (record.Outcome is not (Outcome.Failed or Outcome.Cancelled or Outcome.Interrupted) || !_restarting.Add(jobId)) throw NotRetryable(jobId);
        try
        {
            return await Again(record, fromStep);
        }
        finally
        {
            _restarting.Remove(jobId);
        }
    }

    /// <summary>Retry's same-job part, while the job is held in _restarting.</summary>
    async Task<JobDto> Again(JobRecord record, StepKind? fromStep)
    {
        var jobId = record.Id;
        var steps = (record.Plan?.Steps ?? JobPlanner.Initial(record.Request)).ToList();
        bool keptApart = steps.Contains(StepKind.KeepApart);
        if (keptApart && fromStep != StepKind.KeepApart) throw NotRetryable(jobId);
        int seq = fromStep is { } kind ? steps.IndexOf(kind) : 0;
        if (seq < 0)
            throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.JobUnknownStep), ("step", JsonValue.Of(EnumWire.Name(fromStep!.Value)))));
        var rows = await _executor.Worker(() => _store.Steps().Load(jobId));
        if (seq > JobPlanner.Done(rows).Count) throw BadRequest("fromStep " + EnumWire.Name(fromStep!.Value) + ": a step before it isn't done");
        int release = steps.IndexOf(StepKind.Release);
        if (!keptApart && (release < 0 || seq <= release)) seq = 0;
        if (seq < steps.IndexOf(StepKind.Commit)) await NotCommitting(jobId);
        foreach (var row in rows.Where(r => r.Seq >= seq))
        {
            var pending = row with { State = StepState.Pending, StartedAt = null, FinishedAt = null, Output = null, Error = null };
            await _executor.Worker(() => _store.Steps().Save(pending));
        }
        var plan = record.Plan;
        int planned = steps.IndexOf(StepKind.Plan);
        if (plan != null && planned >= 0 && seq <= planned) plan = plan with { Steps = steps.Take(planned + 1).ToList() };
        var queue = keptApart || (release >= 0 && seq > release) ? Queue.Processing : JobPlanner.Queue(record.Kind);
        var position = Admit(jobId, queue);
        var again = record with
        {
            State = JobState.Queued, Outcome = null, Error = null, FinishedAt = null, Decision = null, BlockedBy = null, Queue = queue, Position = position,
            Plan = plan, Automatic = false, MediaGeneration = null, Request = record.Request with { Automatic = false, MediaGeneration = null },
        };
        try
        {
            await _executor.Worker(() => _store.Jobs().Update(again));
            var line = new LogLineDto(0, _clock.Now(), Severity.Info, MessageCode.JobRetried, (JsonValue.Object)JsonValue.Of(("step", JsonValue.Of(EnumWire.Name(steps[seq])))));
            await _executor.Worker(() =>
            {
                JobLog.Append(_fs, _data + "/jobs/" + jobId.Value, new[] { line });
                return true;
            });
        }
        catch (BroFailure)
        {
            Forget(jobId);
            _scheduler.Finish(jobId);
            throw;
        }
        if (_asked[jobId] != position) SavePosition(jobId);
        var dto = JobDto.Of(again) with { Position = _scheduler.Position(jobId) };
        _bus.Publish(EngineEventType.JobChanged, JsonValue.Of(("job", JobDto.Json(dto))));
        await Launch(again);
        return dto;
    }

    /// <summary>Doesn't continue an interrupted job (owner, 2026-10-03 and 2026-10-25; plan §20.4): the same job runs again
    /// with a keepApart step in place of its interrupted step and what follows (KeepApartStep seals what staging holds and
    /// quarantines it as INCOMPLETE; the job then ends cancelled). The step's row is stored pending (output, error and
    /// checkpoint cleared), the plan cut back to the steps before it plus keepApart, and the record queued again in the
    /// processing queue (no outcome; the request as it was); jobChanged, then it runs. job.notFound; job.notInterrupted for
    /// a job that isn't finished and interrupted, that this service runs, or whose Plan step didn't succeed.</summary>
    public async Task<JobDto> KeepApart(Id jobId)
    {
        var record = await Load(jobId);
        if (record.State != JobState.Finished || record.Outcome != Outcome.Interrupted || _active.ContainsKey(jobId) || !_restarting.Add(jobId))
            throw NotInterrupted(jobId);
        try
        {
            return await KeptApart(record);
        }
        finally
        {
            _restarting.Remove(jobId);
        }
    }

    /// <summary>KeepApart's part, while the job is held in _restarting.</summary>
    async Task<JobDto> KeptApart(JobRecord record)
    {
        var jobId = record.Id;
        var rows = await _executor.Worker(() => _store.Steps().Load(jobId));
        var done = JobPlanner.Done(rows);
        if (!done.Any(r => r.Kind == StepKind.Plan && r.State == StepState.Succeeded)) throw NotInterrupted(jobId);
        int seq = done.Count;
        var steps = (record.Plan?.Steps ?? JobPlanner.Initial(record.Request)).Take(seq).Append(StepKind.KeepApart).ToList();
        var row = new StepRecord(jobId, seq, StepKind.KeepApart, StepState.Pending, 1, null);
        var position = Admit(jobId, Queue.Processing);
        var again = record with
        {
            State = JobState.Queued, Outcome = null, Error = null, FinishedAt = null, Decision = null, BlockedBy = null, Queue = Queue.Processing,
            Position = position, Plan = (record.Plan ?? new JobPlan(steps)) with { Steps = steps },
        };
        try
        {
            await _executor.Worker(() => _store.Steps().Save(row));
            await _executor.Worker(() => _store.Jobs().Update(again));
        }
        catch (BroFailure)
        {
            Forget(jobId);
            _scheduler.Finish(jobId);
            throw;
        }
        if (_asked[jobId] != position) SavePosition(jobId);
        var dto = JobDto.Of(again) with { Position = _scheduler.Position(jobId) };
        _bus.Publish(EngineEventType.JobChanged, JsonValue.Of(("job", JobDto.Json(dto))));
        await Launch(again);
        return dto;
    }

    /// <summary>Answers the job's question <paramref name="decisionId"/>. job.notFound; job.notAwaitingDecision when the job
    /// isn't waiting for that question; server.badRequest for an answer it doesn't offer.</summary>
    public async Task Decide(Id jobId, string decisionId, string answer)
    {
        if (!_active.ContainsKey(jobId)) await Load(jobId);
        if (_runner.Job(jobId)?.Decision?.Id != decisionId)
            throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.JobNotAwaitingDecision), ("jobId", JsonValue.Of(jobId.Value))));
        _runner.Decide(jobId, answer);
    }

    /// <summary>Moves a job that waits to start a step <paramref name="by"/> places in its queue (negative: earlier),
    /// stopping at the ends. job.notFound; job.notQueued for a job that runs a step, waits for an answer or has
    /// finished.</summary>
    public async Task Move(Id jobId, int by)
    {
        await Waiting(jobId);
        _scheduler.Move(jobId, by);
    }

    /// <summary>Puts a job that waits to start a step first in its queue; the acquisitions cap no longer holds it back.
    /// job.notFound; job.notQueued as for <see cref="Move"/>.</summary>
    public async Task StartNow(Id jobId)
    {
        await Waiting(jobId);
        _scheduler.StartNow(jobId);
    }

    /// <summary>A page of jobs, newest first: running jobs as they run (progress, blockedBy, decision), waiting ones with
    /// their place in the queue. The states are the stored ones (queued, running, finished). server.badRequest for a
    /// cursor that isn't one or a limit outside 1–500.</summary>
    public async Task<JobPageDto> Query(JobQueryDto query)
    {
        if (query.Limit < 1 || query.Limit > JobQueryDto.MaxLimit)
            throw BadRequest("limit " + query.Limit.ToString(CultureInfo.InvariantCulture));
        int offset = 0;
        if (query.Cursor != null && !(int.TryParse(query.Cursor, NumberStyles.None, CultureInfo.InvariantCulture, out offset) && offset >= 0))
            throw BadRequest("cursor " + query.Cursor);
        var q = new JobQuery(query.States, query.Outcomes, query.Kind, query.Queue, query.DriveId, query.UnitId, null, query.Limit, offset);
        var (records, total) = await _executor.Worker(async () => (await _store.Jobs().Query(q), await _store.Jobs().Count(q)));
        var items = records.Select(r =>
        {
            var dto = _runner.Job(r.Id) ?? JobDto.Of(r);
            return dto.State is JobState.Queued or JobState.WaitingForResources or JobState.Blocked ? dto with { Position = _scheduler.Position(r.Id) } : dto;
        }).ToList();
        var next = records.Count > 0 && offset + records.Count < total ? (offset + records.Count).ToString(CultureInfo.InvariantCulture) : null;
        return new JobPageDto(items, next, total);
    }

    /// <summary>The job's log lines after seq <paramref name="after"/> (0: all), including a running step's lines not yet
    /// written. job.notFound.</summary>
    public async Task<IReadOnlyList<LogLineDto>> Log(Id jobId, long after)
    {
        var lines = _runner.Log(jobId);
        if (lines == null)
        {
            if (!_active.ContainsKey(jobId)) await Load(jobId);
            var path = _data + "/jobs/" + jobId.Value + "/" + JobLog.FileName;
            lines = await _executor.Worker(() => _fs.Exists(path) ? JobLog.Decode(_fs.Read(path)) : Array.Empty<LogLineDto>());
        }
        return lines.Where(l => l.Seq > after).ToList();
    }

    // ---- running ----

    /// <summary>Puts the job in its queue (in call order: before any await) and in the active view; its place.</summary>
    int Admit(Id jobId, Queue queue)
    {
        _active[jobId] = new Active();
        _scheduler.Submit(jobId, queue);
        var position = _scheduler.Position(jobId) ?? 0;
        _asked[jobId] = position;
        lock (_positions) _positions[jobId] = position;
        return position;
    }

    /// <summary>Starts the job's run (live in the runner at once: a cancel from now on reaches it), or finishes it as
    /// cancelled when a cancel came first.</summary>
    async Task Launch(JobRecord record)
    {
        if (_active[record.Id].CancelRequested)
        {
            try { await FinishCancelled(record); }
            finally
            {
                _scheduler.Finish(record.Id);
                Forget(record.Id);
            }
            return;
        }
        Track(record.Id, _runner.Run(record.Id));
    }

    /// <summary>Finishes a job nothing runs as cancelled (job.cancelledByUser).</summary>
    async Task FinishCancelled(JobRecord record)
    {
        var decided = JobOutcome.Decide(Array.Empty<StepResult>(), true, OutcomePolicy.Default);
        var finished = record with { State = JobState.Finished, Outcome = decided.Outcome, Error = decided.Error, Decision = null, BlockedBy = null,
            FinishedAt = _clock.Now() };
        await _executor.Worker(() => _store.Jobs().Update(finished));
        _bus.Publish(EngineEventType.JobFinished, JsonValue.Of(("job", JobDto.Json(JobDto.Of(finished)))));
    }

    async void Track(Id jobId, Task<Decided> run) // a failure that isn't a BroFailure is a bug, and reaches the executor's handler
    {
        try { await run; }
        catch (BroFailure) { _scheduler.Finish(jobId); } // its record was gone: nothing ran
        finally { Forget(jobId); }
    }

    void Forget(Id jobId)
    {
        _active.Remove(jobId);
        _asked.Remove(jobId);
        lock (_positions) _positions.Remove(jobId);
    }

    /// <summary>Stores the place of every active job whose place in its queue changed.</summary>
    void OnQueueChanged()
    {
        var orders = _scheduler.Queues();
        foreach (var order in new[] { orders.Acquisition, orders.Processing, orders.Maintenance })
            for (int i = 0; i < order.Count; i++)
            {
                var id = order[i];
                if (!_asked.TryGetValue(id, out var asked) || asked == i) continue; // not admitted yet, or in place
                _asked[id] = i;
                lock (_positions) _positions[id] = i;
                SavePosition(id);
            }
    }

    /// <summary>Writes the job's place as it is when the write runs (so a later move wins over an earlier write that lands
    /// last), in a transaction that changes nothing else. Best effort: the order only matters after a restart.</summary>
    async void SavePosition(Id jobId)
    {
        try
        {
            await _executor.Worker(() => _store.Transaction(async store =>
            {
                int position;
                lock (_positions)
                    if (!_positions.TryGetValue(jobId, out position)) return false;
                var record = await store.Jobs().Load(jobId);
                if (record == null || record.State == JobState.Finished || record.Position == position) return false;
                await store.Jobs().Update(record with { Position = position });
                return true;
            }));
        }
        catch (BroFailure) { }
    }

    // ---- helpers ----

    /// <summary>The stored job; job.notFound when there is none.</summary>
    async Task<JobRecord> Load(Id jobId) =>
        await _executor.Worker(() => _store.Jobs().Load(jobId))
        ?? throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.JobNotFound), ("jobId", JsonValue.Of(jobId.Value))));

    /// <summary>job.commitOpen while the job has an open commit intent.</summary>
    async Task NotCommitting(Id jobId)
    {
        if ((await _executor.Worker(() => _store.Units().OpenIntents())).Any(i => i.JobId == jobId))
            throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.JobCommitOpen), ("jobId", JsonValue.Of(jobId.Value))));
    }

    /// <summary>Fails unless the job waits to start a step (queued, waiting for resources, blocked).</summary>
    async Task Waiting(Id jobId)
    {
        if (!_active.ContainsKey(jobId))
        {
            await Load(jobId);
            throw NotQueued(jobId);
        }
        var state = _runner.Job(jobId)?.State ?? JobState.Queued;
        if (state is not (JobState.Queued or JobState.WaitingForResources or JobState.Blocked) || _scheduler.Position(jobId) == null) throw NotQueued(jobId);
    }

    static BroFailure NotRetryable(Id jobId) => new(new BroError(MessageCode.Wire(MessageCode.JobNotRetryable), ("jobId", JsonValue.Of(jobId.Value))));

    static BroFailure NotInterrupted(Id jobId) => new(new BroError(MessageCode.Wire(MessageCode.JobNotInterrupted), ("jobId", JsonValue.Of(jobId.Value))));

    static BroFailure NotQueued(Id jobId) => new(new BroError(MessageCode.Wire(MessageCode.JobNotQueued), ("jobId", JsonValue.Of(jobId.Value))));

    static BroFailure BadRequest(string detail) => new(new BroError(MessageCode.Wire(MessageCode.ServerBadRequest), ("detail", JsonValue.Of(detail))));
}
