using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Engine;

/// <summary>
/// The only code that moves a job between states (plan §12.3), on the executor. Today's JobRunner.run()/execute()
/// becomes JobPlanner (which steps), the step classes (what each does), JobOutcome.Decide (the result) and this loop
/// (the order, the resources, and the durable record of every step). shared/fixtures/engine/pipeline-runner.cases.json.
/// </summary>
public sealed class PipelineRunner
{
    readonly EngineExecutor _executor;
    readonly EventBus _bus;
    readonly Scheduler _scheduler;
    readonly IStore _store;
    readonly StepRegistry _registry;
    readonly StepEnvironment _env;
    readonly EngineClock _clock;
    readonly Coalescer _progress;
    readonly Dictionary<Id, Live> _live = new();

    /// <summary>A job while it runs. Touched on the executor only.</summary>
    sealed class Live
    {
        /// <summary>Loaded when the run starts; null until then (the job is already live: it can be cancelled).</summary>
        public JobRecord Record = null!;
        public readonly CancellationSource Cancel = new();
        public List<StepKind> Steps = new();
        public int Seq = -1;
        public ProgressDto? Progress;
        public ProgressDto? PendingProgress;
        public BroMessage? BlockedBy;
        public (DecisionDto Question, TaskCompletionSource<string> Answer)? Decision;
        public long LogSeq;
        /// <summary>The log file as an earlier run (or recovery) left it; this run's lines follow.</summary>
        public byte[] LogBefore = Array.Empty<byte>();
        public readonly List<LogLineDto> Log = new();
        public bool DirectoryMade;
        public bool LogRead;
        public Config? Config;
        /// <summary>The outputs of the steps done so far, for the Release step at the end.</summary>
        public List<StepOutput> Outputs = new();
        /// <summary>Each step kind's latest checkpoint (ICheckpointer): from the stored rows, then what this run saves.</summary>
        public readonly Dictionary<StepKind, JsonValue> Checkpoints = new();
    }

    public PipelineRunner(EngineExecutor executor, EventBus bus, Scheduler scheduler, IStore store, StepRegistry registry,
        StepEnvironment environment, EngineClock clock)
    {
        _executor = executor;
        _bus = bus;
        _scheduler = scheduler;
        _store = store;
        _registry = registry;
        _env = environment;
        _clock = clock;
        _progress = new Coalescer(clock, new Duration(0.1), (key, _) => EmitProgress(new Id(key)));
    }

    // ---- commands ----

    /// <summary>Runs the job to its end, or until it is cancelled; returns how it ended. The job is live (cancel reaches
    /// it) as soon as this is called, before its record is loaded. job.notFound when there is no such job.</summary>
    public async Task<Decided> Run(Id jobId)
    {
        var live = new Live();
        _live[jobId] = live;
        try
        {
            live.Record = await _executor.Worker(() => _store.Jobs().Load(jobId))
                ?? throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.JobNotFound), ("jobId", JsonValue.Of(jobId.Value))));
        }
        catch (BroFailure)
        {
            _live.Remove(jobId);
            live.Cancel.Close();
            throw;
        }
        Decided decided;
        try
        {
            decided = await RunSteps(live);
        }
        catch (BroFailure f)
        {
            decided = new Decided(Outcome.Failed, f.Error);
        }
        catch (Exception e)
        {
            decided = new Decided(Outcome.Failed, Unexpected(e.Message));
        }
        await ReleaseAtEnd(live, decided);
        await Finish(live, decided);
        _live.Remove(jobId);
        live.Cancel.Close();
        return decided;
    }

    /// <summary>Cancels a running job: a waiting request or a decision ends at once, a running step sees its token.</summary>
    public void Cancel(Id jobId)
    {
        if (!_live.TryGetValue(jobId, out var live)) return;
        live.Cancel.Cancel();
        if (live.Decision is { } d)
        {
            live.Decision = null;
            d.Answer.TrySetException(new BroFailure(new BroError(MessageCode.Wire(MessageCode.JobCancelled))));
        }
    }

    /// <summary>The job as it runs (progress, blockedBy, decision); null when it isn't running here or its record isn't
    /// loaded yet.</summary>
    public JobDto? Job(Id jobId) => _live.TryGetValue(jobId, out var live) && live.Record != null ? Dto(live) : null;

    /// <summary>The running job's log: what the file held when it started, then this run's lines, written or not; null when
    /// it isn't running here or hasn't read its log yet.</summary>
    public IReadOnlyList<LogLineDto>? Log(Id jobId) =>
        _live.TryGetValue(jobId, out var live) && live.LogRead ? JobLog.Decode(live.LogBefore).Concat(live.Log).ToList() : null;

    /// <summary>Answers the job's question. job.notAwaitingDecision when it has none; server.badRequest for an answer
    /// the question doesn't offer.</summary>
    public void Decide(Id jobId, string answer)
    {
        if (!_live.TryGetValue(jobId, out var live) || live.Decision is not { } d)
            throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.JobNotAwaitingDecision), ("jobId", JsonValue.Of(jobId.Value))));
        if (d.Question.Answers.All(a => a.Id != answer))
            throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.ServerBadRequest), ("detail", JsonValue.Of("answer " + answer))));
        live.Decision = null;
        SetState(live, JobState.Running);
        d.Answer.TrySetResult(answer);
    }

    // ---- the loop ----

    async Task<Decided> RunSteps(Live live)
    {
        var jobId = live.Record.Id;
        var plan = live.Record.Plan;
        if (plan == null)
        {
            var initial = JobPlanner.Initial(live.Record.Request);
            if (initial.Count == 0) throw new BroFailure(Unexpected("no steps for a " + EnumWire.Name(live.Record.Kind) + " job yet"));
            plan = new JobPlan(initial);
            await SaveRecord(live, live.Record with { Plan = plan });
        }
        live.Steps = plan.Steps.ToList();
        live.Config = _env.Config.Current();
        var logFile = JobDirectory(jobId) + "/" + JobLog.FileName;
        live.LogBefore = await _executor.Worker(() => _env.Fs.Exists(logFile) ? _env.Fs.Read(logFile) : Array.Empty<byte>());
        live.LogSeq = live.LogBefore.Count(b => b == (byte)'\n');
        live.LogRead = true;
        live.DirectoryMade = live.LogBefore.Length > 0;
        var rows = await _executor.Worker(() => _store.Steps().Load(jobId));
        foreach (var row in rows.Where(r => r.Checkpoint != null).OrderBy(r => r.Seq)) live.Checkpoints[row.Kind] = row.Checkpoint!;
        var done = JobPlanner.Done(rows);
        var outputs = live.Outputs = done.Select(s => s.Output ?? new StepOutput(s.Kind, Empty)).ToList();
        var results = done.Select(s => new StepResult(s.Kind, s.State, s.Error, s.Output?.ReadErrors ?? 0)).ToList();
        _scheduler.Submit(jobId, live.Record.Queue);
        bool cancelled = false;
        for (int seq = done.Count; seq < live.Steps.Count; seq++)
        {
            if (live.Cancel.Token.IsCancelled) // cancelled between steps, or before the first
            {
                cancelled = true;
                break;
            }
            var kind = live.Steps[seq];
            var step = _registry.Step(kind) ?? throw new BroFailure(Unexpected("no step " + EnumWire.Name(kind)));
            live.Seq = seq;
            _progress.Drop(jobId.Value); // a new step's first progress goes out at once
            live.Progress = new ProgressDto(kind, seq, live.Steps.Count, (double)seq / live.Steps.Count, null,
                new BroMessage(MessageCode.JobOpStarting));
            var snapshot = new JobSnapshot(jobId, live.Record.Request, plan with { Steps = live.Steps.ToList() }, outputs.ToList(), live.Record.StartedAt);
            Lease lease;
            try
            {
                lease = await _scheduler.Acquire(jobId, step.Resources(snapshot), wait => OnWait(live, wait), live.Cancel.Token);
            }
            catch (BroFailure f) when (f.Error.Code == MessageCode.Wire(MessageCode.JobCancelled))
            {
                cancelled = true;
                break;
            }
            live.BlockedBy = null;
            var started = _clock.Now();
            await SaveRecord(live, live.Record with { State = JobState.Running, StartedAt = live.Record.StartedAt ?? started });
            SetState(live, JobState.Running);
            var running = new StepRecord(jobId, seq, kind, StepState.Running, 1, started, Checkpoint: live.Checkpoints.GetValueOrDefault(kind));
            await _executor.Worker(() => _store.Steps().Save(running));

            StepReport? report = null;
            BroError? thrown = null;
            try
            {
                var context = Context(live, snapshot, seq, new Checkpoints(this, live, running));
                report = await _executor.Worker(() => step.Run(context));
            }
            catch (BroFailure f)
            {
                thrown = f.Error;
            }
            catch (Exception e)
            {
                thrown = Unexpected(e.Message);
            }
            _scheduler.Release(lease);
            _progress.Flush(jobId.Value);

            var error = thrown ?? report?.Error;
            var state = error != null ? (live.Cancel.Token.IsCancelled ? StepState.Cancelled : StepState.Failed)
                : report!.Skipped ? StepState.Skipped : StepState.Succeeded;
            var output = new StepOutput(kind, report?.Data ?? Empty, report?.Summary, report?.ReadErrors ?? 0);
            await _executor.Worker(() => _store.Steps().Save(new StepRecord(jobId, seq, kind, state, 1, started, _clock.Now(), output, error,
                live.Checkpoints.GetValueOrDefault(kind))));
            _bus.Publish(EngineEventType.StepFinished, JsonValue.Of(("job", JsonValue.Of(jobId.Value)), ("seq", JsonValue.Of(seq)),
                ("kind", JsonValue.Of(EnumWire.Name(kind))), ("state", JsonValue.Of(EnumWire.Name(state)))));
            await WriteLog(live);
            outputs.Add(output);
            results.Add(new StepResult(kind, state, error, report?.ReadErrors ?? 0, report?.Quarantined ?? false, report?.Notice,
                report?.SkipJob, report?.AffectsOutcome ?? true));

            if (kind == StepKind.Release && state == StepState.Succeeded)
            {
                _scheduler.ReleaseDrives(jobId);
                _scheduler.MoveTo(jobId, Queue.Processing);
                live.Record = live.Record with { Queue = Queue.Processing };
            }
            if (state == StepState.Cancelled)
            {
                cancelled = true;
                break;
            }
            if (state == StepState.Failed && !JobOutcome.AfterCommit(kind)) break;
            if (report?.Plan != null || report?.AppendSteps is { Count: > 0 })
            {
                if (report.AppendSteps != null) live.Steps.AddRange(report.AppendSteps);
                plan = (report.Plan ?? plan) with { Steps = live.Steps.ToList() };
                await SaveRecord(live, live.Record with { Plan = plan, Mode = plan.Mode ?? live.Record.Mode });
            }
            if (report?.SkipJob != null) break; // the job is skipped: nothing after this step runs
        }
        return JobOutcome.Decide(results, cancelled || live.Cancel.Token.IsCancelled, OutcomePolicy.Default);
    }

    /// <summary>A job that ends while it still holds its drive (it stopped before its Release step: failed, skipped,
    /// cancelled) runs the Release step once more, with its outcome as the context's Finishing: the step decides whether
    /// the disc comes out (owner, 2026-10-18). Nothing is stored for it but its log lines; what it throws is ignored.</summary>
    async Task ReleaseAtEnd(Live live, Decided decided)
    {
        if (live.Record == null || !live.LogRead || _registry.Step(StepKind.Release) is not { } release) return;
        var jobId = live.Record.Id;
        if (!_scheduler.Leases().Drives.Any(h => h.Job == jobId)) return;
        var snapshot = new JobSnapshot(jobId, live.Record.Request, live.Record.Plan, live.Outputs.ToList(), live.Record.StartedAt);
        var context = Context(live, snapshot, live.Seq, Silent.Instance) with { Progress = Silent.Instance, Finishing = decided };
        try
        {
            await _executor.Worker(() => release.Run(context));
        }
        catch (Exception)
        {
            // The job ends either way; the step logs a failed eject itself.
        }
        await WriteLog(live);
    }

    async Task Finish(Live live, Decided decided)
    {
        var jobId = live.Record.Id;
        live.Progress = null;
        live.PendingProgress = null;
        live.BlockedBy = null;
        live.Decision = null;
        var record = live.Record with { State = JobState.Finished, Outcome = decided.Outcome, Error = decided.Error, FinishedAt = _clock.Now() };
        live.Record = record;
        try
        {
            await _executor.Worker(() => _store.Jobs().Update(record));
        }
        catch (BroFailure)
        {
            // The job has ended either way; what the store couldn't keep, recovery will see as unfinished.
        }
        _progress.Drop(jobId.Value);
        _scheduler.Finish(jobId);
        _bus.Publish(EngineEventType.JobFinished, JsonValue.Of(("job", JobDto.Json(Dto(live)))));
    }

    // ---- state and events ----

    /// <summary>Stores the record, with the job's place in its queue as it is now (JobService stores it when it changes;
    /// a stale one here would undo that).</summary>
    async Task SaveRecord(Live live, JobRecord record)
    {
        live.Record = record = record with { Position = _scheduler.Position(record.Id) ?? record.Position };
        await _executor.Worker(() => _store.Jobs().Update(record));
    }

    void SetState(Live live, JobState state)
    {
        live.Record = live.Record with { State = state };
        _bus.Publish(EngineEventType.JobChanged, JsonValue.Of(("job", JobDto.Json(Dto(live)))));
    }

    void OnWait(Live live, Wait wait)
    {
        if (wait is Wait.Blocked b)
        {
            live.BlockedBy = new BroMessage(MessageCode.Parse(b.Reason.Code) ?? MessageCode.InternalUnexpected, b.Reason.Params);
            SetState(live, JobState.Blocked);
        }
        else
        {
            live.BlockedBy = null;
            SetState(live, JobState.WaitingForResources);
        }
    }

    JobDto Dto(Live live) => JobDto.Of(live.Record) with { Progress = live.Progress, BlockedBy = live.BlockedBy, Decision = live.Decision?.Question };

    void OnProgress(Live live, int seq, double fraction, BroMessage operation, string? current)
    {
        if (live.Seq != seq || live.Record.State == JobState.Finished) return;
        var f = Math.Clamp(fraction, 0, 1);
        live.PendingProgress = new ProgressDto(live.Steps[seq], seq, live.Steps.Count, (seq + f) / live.Steps.Count, f, operation, current);
        _progress.Offer(live.Record.Id.Value, JsonValue.Null.Instance);
    }

    void EmitProgress(Id jobId)
    {
        if (!_live.TryGetValue(jobId, out var live) || live.PendingProgress is not { } p) return;
        live.Progress = p;
        live.PendingProgress = null;
        SetState(live, live.Record.State);
    }

    void OnLog(Live live, BroMessage message)
    {
        var line = new LogLineDto(++live.LogSeq, _clock.Now(), message.Severity, message.Code, message.Params);
        live.Log.Add(line);
        _bus.Publish(EngineEventType.JobLog, JsonValue.Of(("jobId", JsonValue.Of(live.Record.Id.Value)),
            ("lines", JsonValue.Of(new[] { LogLineDto.Json(line) }))));
    }

    void OnAsk(Live live, DecisionDto question, TaskCompletionSource<string> answer)
    {
        if (live.Cancel.Token.IsCancelled)
        {
            answer.TrySetException(new BroFailure(new BroError(MessageCode.Wire(MessageCode.JobCancelled))));
            return;
        }
        live.Decision = (question, answer);
        SetState(live, JobState.AwaitingDecision);
    }

    /// <summary>jobs/&lt;id&gt;/log.jsonl (JobLog): what was there when the job started, then this run's lines, written
    /// whole after each step.</summary>
    async Task WriteLog(Live live)
    {
        if (live.Log.Count == 0) return;
        var directory = JobDirectory(live.Record.Id);
        var bytes = live.LogBefore.Concat(JobLog.Encode(live.Log)).ToArray();
        var makeDirectory = !live.DirectoryMade;
        live.DirectoryMade = true;
        await _executor.Worker(() =>
        {
            if (makeDirectory) _env.Fs.CreateDirectory(directory, parentsMustExist: false);
            _env.Fs.WriteAtomically(directory + "/" + JobLog.FileName, bytes, 0x180); // 0600
            return true;
        });
    }

    string JobDirectory(Id jobId) => _env.DataDirectory + "/jobs/" + jobId.Value;

    StepContext Context(Live live, JobSnapshot snapshot, int seq, ICheckpointer checkpoint) => new(snapshot, live.Cancel.Token,
        new Reporter(this, live, seq), new Logger(this, live), new Decisions(this, live), _env.Tools, _env.Fs, _env.Clock, _env.Drives,
        _env.Library, _env.Catalog, live.Config!, _env.DriveLookup, _env.MakemkvSetup, new Space(this, live), checkpoint, JobDirectory(live.Record.Id),
        _env.Messages, _env.AppVersion);

    static readonly JsonValue Empty = JsonValue.Of(Array.Empty<(string, JsonValue)>());

    static BroError Unexpected(string detail) =>
        new(MessageCode.Wire(MessageCode.InternalUnexpected), ("detail", JsonValue.Of(detail)));

    // ---- what a step's context hands back to the executor ----

    sealed class Reporter : IProgressReporter
    {
        readonly PipelineRunner _runner;
        readonly Live _live;
        readonly int _seq;

        public Reporter(PipelineRunner runner, Live live, int seq)
        {
            _runner = runner;
            _live = live;
            _seq = seq;
        }

        public void Report(double fraction, BroMessage operation, string? current = null) =>
            _runner._executor.Post(() => _runner.OnProgress(_live, _seq, fraction, operation, current));
    }

    sealed class Logger : IJobLogger
    {
        readonly PipelineRunner _runner;
        readonly Live _live;

        public Logger(PipelineRunner runner, Live live)
        {
            _runner = runner;
            _live = live;
        }

        public void Log(BroMessage message) => _runner._executor.Post(() => _runner.OnLog(_live, message));
    }

    /// <summary>The Release step at the end of a job: no progress, and no checkpoint (nothing is stored for it).</summary>
    sealed class Silent : IProgressReporter, ICheckpointer
    {
        public static readonly Silent Instance = new();

        public void Report(double fraction, BroMessage operation, string? current = null)
        {
        }

        public JsonValue? Saved => null;

        public Task Save(JsonValue checkpoint) => Task.CompletedTask;
    }

    /// <summary>A step's checkpoint: kept on the executor and stored with the step's running row; the row it finishes with
    /// keeps the last one (a retry of the job hands it to the step again).</summary>
    sealed class Checkpoints : ICheckpointer
    {
        readonly PipelineRunner _runner;
        readonly Live _live;
        readonly StepRecord _running;

        public Checkpoints(PipelineRunner runner, Live live, StepRecord running)
        {
            _runner = runner;
            _live = live;
            _running = running;
            Saved = running.Checkpoint;
        }

        public JsonValue? Saved { get; }

        public Task Save(JsonValue checkpoint)
        {
            var done = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
            if (!_runner._executor.Post(async () =>
            {
                _live.Checkpoints[_running.Kind] = checkpoint;
                try
                {
                    await _runner._executor.Worker(() => _runner._store.Steps().Save(_running with { Checkpoint = checkpoint }));
                    done.TrySetResult(true);
                }
                catch (Exception e) // a store that fails fails the step, which awaits it
                {
                    done.TrySetException(e);
                }
            }))
                done.TrySetException(new BroFailure(new BroError(MessageCode.Wire(MessageCode.JobCancelled))));
            return done.Task;
        }
    }

    sealed class Space : ISpaceReservations
    {
        readonly PipelineRunner _runner;
        readonly Live _live;

        public Space(PipelineRunner runner, Live live)
        {
            _runner = runner;
            _live = live;
        }

        public Task Reserve(string volume, string path, long need, long free)
        {
            var done = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
            if (!_runner._executor.Post(() =>
            {
                try
                {
                    _runner._scheduler.Reserve(_live.Record.Id, volume, path, need, free);
                    done.TrySetResult(true);
                }
                catch (BroFailure f)
                {
                    done.TrySetException(f);
                }
            }))
                done.TrySetException(new BroFailure(new BroError(MessageCode.Wire(MessageCode.JobCancelled))));
            return done.Task;
        }
    }

    sealed class Decisions : IDecisionPort
    {
        readonly PipelineRunner _runner;
        readonly Live _live;

        public Decisions(PipelineRunner runner, Live live)
        {
            _runner = runner;
            _live = live;
        }

        public Task<string> Ask(DecisionDto decision)
        {
            var answer = new TaskCompletionSource<string>(TaskCreationOptions.RunContinuationsAsynchronously);
            if (!_runner._executor.Post(() => _runner.OnAsk(_live, decision, answer)))
                answer.TrySetException(new BroFailure(new BroError(MessageCode.Wire(MessageCode.JobCancelled))));
            return answer.Task;
        }
    }
}
