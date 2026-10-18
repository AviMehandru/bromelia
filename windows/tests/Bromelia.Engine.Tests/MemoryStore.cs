using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Engine.Tests;

/// <summary>Jobs and steps in memory (the rest of the store isn't needed by these tests). Thread-safe.</summary>
internal sealed class MemoryStore : IStore, IJobRepository, IStepRepository
{
    readonly object _lock = new();
    readonly Dictionary<Id, JobRecord> _jobs = new();
    readonly Dictionary<(Id, int), StepRecord> _steps = new();

    public IJobRepository Jobs() => this;
    public IStepRepository Steps() => this;
    public IUnitRepository Units() => throw new NotSupportedException();
    public ICatalogRepository Catalog() => throw new NotSupportedException();
    public ICheckRepository Checks() => throw new NotSupportedException();
    public IReplicaRepository Replicas() => throw new NotSupportedException();
    public IDriveRepository Drives() => throw new NotSupportedException();
    public ILibraryRepository Libraries() => throw new NotSupportedException();
    public ILookupCacheRepository LookupCache() => throw new NotSupportedException();
    public IOutboxRepository Outbox() => throw new NotSupportedException();
    public IKeyValueRepository Kv() => throw new NotSupportedException();
    public Task<T> Transaction<T>(Func<IStore, Task<T>> block) => block(this);
    public Task Migrate() => Task.CompletedTask;

    Task IJobRepository.Insert(JobRecord job)
    {
        lock (_lock) _jobs[job.Id] = job;
        return Task.CompletedTask;
    }

    Task IJobRepository.Update(JobRecord job)
    {
        lock (_lock) _jobs[job.Id] = job;
        return Task.CompletedTask;
    }

    Task<JobRecord?> IJobRepository.Load(Id jobId)
    {
        lock (_lock) return Task.FromResult(_jobs.GetValueOrDefault(jobId));
    }

    Task<IReadOnlyList<JobRecord>> IJobRepository.Active()
    {
        lock (_lock)
            return Task.FromResult<IReadOnlyList<JobRecord>>(_jobs.Values.Where(j => j.State != JobState.Finished)
                .OrderBy(j => j.Queue switch { Queue.Acquisition => 0, Queue.Processing => 1, _ => 2 }).ThenBy(j => j.Position)
                .ThenBy(j => j.CreatedAt.UnixMilliseconds).ThenBy(j => j.Id.Value, StringComparer.Ordinal).ToList());
    }

    Task<IReadOnlyList<JobRecord>> IJobRepository.Query(JobQuery query)
    {
        lock (_lock) return Task.FromResult<IReadOnlyList<JobRecord>>(Matching(query).Skip(query.Offset).Take(query.Limit).ToList());
    }

    Task<int> IJobRepository.Count(JobQuery query)
    {
        lock (_lock) return Task.FromResult(Matching(query).Count());
    }

    /// <summary>What SqliteStore's query matches, newest first.</summary>
    IEnumerable<JobRecord> Matching(JobQuery q) => _jobs.Values
        .Where(j => (q.States.Count == 0 || q.States.Contains(j.State)) && (q.Outcomes.Count == 0 || (j.Outcome is { } o && q.Outcomes.Contains(o)))
                    && (q.Kind == null || j.Kind == q.Kind) && (q.Queue == null || j.Queue == q.Queue) && (q.DriveId == null || j.DriveId == q.DriveId)
                    && (q.UnitId == null || j.UnitId == q.UnitId)
                    && (q.FinishedAfter == null || (j.FinishedAt is { } f && f.UnixMilliseconds > q.FinishedAfter.Value.UnixMilliseconds)))
        .OrderByDescending(j => j.CreatedAt.UnixMilliseconds).ThenByDescending(j => j.Id.Value, StringComparer.Ordinal).ToList();

    Task IStepRepository.Save(StepRecord step)
    {
        lock (_lock) _steps[(step.JobId, step.Seq)] = step;
        return Task.CompletedTask;
    }

    Task<IReadOnlyList<StepRecord>> IStepRepository.Completed(Id jobId)
    {
        lock (_lock)
            return Task.FromResult<IReadOnlyList<StepRecord>>(_steps.Values.Where(s => s.JobId == jobId && s.State == StepState.Succeeded)
                .OrderBy(s => s.Seq).ToList());
    }

    Task<IReadOnlyList<StepRecord>> IStepRepository.Load(Id jobId)
    {
        lock (_lock) return Task.FromResult<IReadOnlyList<StepRecord>>(_steps.Values.Where(s => s.JobId == jobId).OrderBy(s => s.Seq).ToList());
    }

    public JobRecord? Job(Id id)
    {
        lock (_lock) return _jobs.GetValueOrDefault(id);
    }

    public List<StepRecord> StepsOf(Id id)
    {
        lock (_lock) return _steps.Values.Where(s => s.JobId == id).OrderBy(s => s.Seq).ToList();
    }
}
