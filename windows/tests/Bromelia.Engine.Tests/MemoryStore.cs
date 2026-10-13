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
        lock (_lock) return Task.FromResult<IReadOnlyList<JobRecord>>(_jobs.Values.Where(j => j.State != JobState.Finished).ToList());
    }

    Task<IReadOnlyList<JobRecord>> IJobRepository.Query(JobQuery query)
    {
        lock (_lock) return Task.FromResult<IReadOnlyList<JobRecord>>(_jobs.Values.ToList());
    }

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
