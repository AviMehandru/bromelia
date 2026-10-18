using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Engine;

/// <summary>
/// The three queues, the resources steps ask for, and space reservations (plan §21), on the executor. Replaces
/// today's <c>AppState.Pump</c> (one running job per drive lane, maxConcurrentJobs) and <c>BackgroundQueue</c>
/// (backgroundJobs at once). shared/fixtures/engine/scheduler.cases.json.
/// <list type="bullet">
/// <item>A drive has room for one job, and a job keeps a drive it was given until <see cref="ReleaseDrives"/> or
/// <see cref="Finish"/>; a step's other resources go back on <see cref="Release"/>.</item>
/// <item>Acquisition slots: limits.acquisitions (0 = no cap; a job put first by <see cref="StartNow"/> isn't held
/// back by it). Library writes: unlimited, blocked while the library is offline. Cpu: limits.cpu. Io per volume:
/// limits.ioPerVolume. Every request of a job in the processing queue also takes a processing slot
/// (limits.processing) while its step runs. A maintenance job waits while any acquisition slot is held.</item>
/// <item>Requests are considered by queue, then the job's place in it, then request order. A request that has to
/// wait keeps later ones from overtaking it only on the resources it waits for.</item>
/// </list>
/// </summary>
public sealed class Scheduler
{
    const string AcquisitionKey = "acquisition";
    const string ProcessingKey = "processing";
    const string CpuKey = "cpu";

    sealed class Request
    {
        public required Id Job;
        public required ResourceSet Resources;
        public required Action<Wait> OnWait;
        public required TaskCompletionSource<Lease> Done;
        public long Order;
        public Wait? LastWait;
        public IDisposable? CancelRegistration;
    }

    readonly EngineExecutor _executor;
    readonly EventBus _bus;
    SchedulerLimits _limits;
    readonly Dictionary<Queue, List<Id>> _queues = new()
    {
        [Queue.Acquisition] = new(), [Queue.Processing] = new(), [Queue.Maintenance] = new(),
    };
    readonly Dictionary<Id, Queue> _queueOf = new();
    readonly HashSet<Id> _startNow = new();
    readonly List<Request> _pending = new();
    readonly Dictionary<string, Id> _driveHolder = new();
    readonly Dictionary<string, int> _held = new();
    readonly Dictionary<Id, List<Lease>> _leases = new();
    readonly Dictionary<string, BroError> _offline = new();
    readonly Dictionary<(Id Job, string Volume), long> _reserved = new();
    LeaseSnapshot _published = LeaseSnapshot.Empty;
    long _nextOrder;
    long _granted;

    public Scheduler(EngineExecutor executor, EventBus bus, SchedulerLimits limits)
    {
        _executor = executor;
        _bus = bus;
        _limits = limits;
    }

    // ---- queues ----

    public void Submit(Id jobId, Queue queue)
    {
        if (_queueOf.ContainsKey(jobId)) return;
        _queueOf[jobId] = queue;
        _queues[queue].Add(jobId);
        PublishQueue(queue);
        Pump();
    }

    /// <summary>Moves a job to the end of another queue (after Release: processing).</summary>
    public void MoveTo(Id jobId, Queue queue)
    {
        if (!_queueOf.TryGetValue(jobId, out var from) || from == queue) return;
        _queues[from].Remove(jobId);
        _queueOf[jobId] = queue;
        _queues[queue].Add(jobId);
        PublishQueue(from);
        PublishQueue(queue);
        Pump();
    }

    /// <summary>Moves a job within its queue, stopping at the ends.</summary>
    public void Move(Id jobId, int by)
    {
        var order = QueueOfOrFail(jobId);
        int from = order.IndexOf(jobId), to = Math.Clamp(from + by, 0, order.Count - 1);
        if (from == to) return;
        order.RemoveAt(from);
        order.Insert(to, jobId);
        PublishQueue(_queueOf[jobId]);
        Pump();
    }

    /// <summary>Puts a job first in its queue; from now on the acquisitions cap doesn't hold it back.</summary>
    public void StartNow(Id jobId)
    {
        var order = QueueOfOrFail(jobId);
        _startNow.Add(jobId);
        if (order[0] != jobId)
        {
            order.Remove(jobId);
            order.Insert(0, jobId);
            PublishQueue(_queueOf[jobId]);
        }
        Pump();
    }

    List<Id> QueueOfOrFail(Id jobId) =>
        _queueOf.TryGetValue(jobId, out var q) ? _queues[q]
            : throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.JobNotQueued), ("jobId", JsonValue.Of(jobId.Value))));

    /// <summary>Cancels the job's waiting requests, gives back everything it holds and its space, and takes it out of
    /// its queue.</summary>
    public void Finish(Id jobId)
    {
        foreach (var r in _pending.Where(r => r.Job == jobId).ToList()) Cancel(r);
        if (_leases.Remove(jobId, out var leases))
            foreach (var lease in leases) GiveBack(lease);
        foreach (var d in _driveHolder.Where(h => h.Value == jobId).Select(h => h.Key).ToList()) _driveHolder.Remove(d);
        Unreserve(jobId);
        _startNow.Remove(jobId);
        if (_queueOf.Remove(jobId, out var q))
        {
            _queues[q].Remove(jobId);
            PublishQueue(q);
        }
        Pump();
    }

    /// <summary>The job's place in its queue (0 is first); null when it is in none.</summary>
    public int? Position(Id jobId) => _queueOf.TryGetValue(jobId, out var q) ? _queues[q].IndexOf(jobId) : null;

    public QueueOrders Queues() =>
        new(_queues[Queue.Acquisition].ToList(), _queues[Queue.Processing].ToList(), _queues[Queue.Maintenance].ToList());

    // ---- leases ----

    /// <summary>Asks for <paramref name="resources"/>; granted, never inside the call, when everything has room.
    /// <paramref name="onWait"/> hears each change while the request waits. Fails with job.cancelled when cancelled
    /// (or the job finishes) first, and with job.notQueued for a job in no queue.</summary>
    public Task<Lease> Acquire(Id jobId, ResourceSet resources, Action<Wait> onWait, CancellationToken cancel)
    {
        QueueOfOrFail(jobId);
        var request = new Request
        {
            Job = jobId, Resources = resources, OnWait = onWait, Order = _nextOrder++,
            Done = new TaskCompletionSource<Lease>(TaskCreationOptions.RunContinuationsAsynchronously),
        };
        _pending.Add(request);
        request.CancelRegistration = cancel.OnCancel(() => _executor.Post(() =>
        {
            if (_pending.Contains(request))
            {
                Cancel(request);
                Pump();
            }
        }));
        Pump();
        return request.Done.Task;
    }

    void Cancel(Request request)
    {
        _pending.Remove(request);
        request.CancelRegistration?.Dispose();
        request.Done.TrySetException(new BroFailure(new BroError(MessageCode.Wire(MessageCode.JobCancelled))));
    }

    /// <summary>Cancels every waiting request of the jobs in <paramref name="queue"/>.</summary>
    public void CancelAll(Queue queue)
    {
        foreach (var r in _pending.Where(r => _queueOf.GetValueOrDefault(r.Job) == queue).ToList()) Cancel(r);
        Pump();
    }

    /// <summary>Gives back a lease, but not its drives. Twice does nothing.</summary>
    public void Release(Lease lease)
    {
        if (lease.Released) return;
        GiveBack(lease);
        if (_leases.TryGetValue(lease.JobId, out var list))
        {
            list.Remove(lease);
            if (list.Count == 0) _leases.Remove(lease.JobId);
        }
        Pump();
    }

    void GiveBack(Lease lease)
    {
        lease.Released = true;
        foreach (var (key, n) in lease.Counted) _held[key] = _held.GetValueOrDefault(key) - n;
    }

    /// <summary>Gives back the job's drives (the Release step).</summary>
    public void ReleaseDrives(Id jobId)
    {
        foreach (var d in _driveHolder.Where(h => h.Value == jobId).Select(h => h.Key).ToList()) _driveHolder.Remove(d);
        Pump();
    }

    public void SetLibraryOffline(string libraryId, BroError? reason)
    {
        if (reason is null) _offline.Remove(libraryId);
        else _offline[libraryId] = reason;
        Pump();
    }

    public void UpdateLimits(SchedulerLimits limits)
    {
        _limits = limits;
        Pump();
    }

    public LeaseSnapshot Leases()
    {
        var drives = _driveHolder.OrderBy(h => h.Key, StringComparer.Ordinal).Select(h => new DriveHolder(h.Key, h.Value)).ToList();
        var holding = _leases.Keys.Concat(_driveHolder.Values).Distinct().Where(_queueOf.ContainsKey).ToList();
        int Count(Queue q) => holding.Count(j => _queueOf[j] == q);
        return new LeaseSnapshot(drives, _held.GetValueOrDefault(AcquisitionKey),
            new JobCounts(Count(Queue.Acquisition), Count(Queue.Processing), Count(Queue.Maintenance)));
    }

    // ---- space ----

    /// <summary>Keeps <paramref name="need"/> bytes on <paramref name="volume"/> for the job, replacing what it kept
    /// there. space.insufficient when <paramref name="free"/> minus what other jobs keep there is less.</summary>
    public void Reserve(Id jobId, string volume, string path, long need, long free)
    {
        long others = _reserved.Where(r => r.Key.Volume == volume && r.Key.Job != jobId).Sum(r => r.Value);
        long available = Math.Max(0, free - others);
        if (available < need)
            throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.SpaceInsufficient),
                ("path", JsonValue.Of(path)), ("need", JsonValue.Of(need)), ("free", JsonValue.Of(available))));
        _reserved[(jobId, volume)] = need;
    }

    public void Unreserve(Id jobId)
    {
        foreach (var key in _reserved.Keys.Where(k => k.Job == jobId).ToList()) _reserved.Remove(key);
    }

    // ---- granting ----

    static int Rank(Queue q) => q switch { Queue.Acquisition => 0, Queue.Processing => 1, _ => 2 };

    int Capacity(string key, Id job) => key switch
    {
        AcquisitionKey => _limits.Acquisitions == 0 || _startNow.Contains(job) ? int.MaxValue : _limits.Acquisitions,
        ProcessingKey => Math.Max(1, _limits.Processing),
        CpuKey => Math.Max(1, _limits.Cpu),
        _ => Math.Max(1, _limits.IoPerVolume), // io:<volume>
    };

    List<(string Key, int Count)> Counted(Request r)
    {
        var needs = new List<(string, int)>();
        if (r.Resources.Acquisition) needs.Add((AcquisitionKey, 1));
        if (_queueOf[r.Job] == Queue.Processing) needs.Add((ProcessingKey, 1));
        if (r.Resources.Cpu > 0) needs.Add((CpuKey, Math.Min(r.Resources.Cpu, Capacity(CpuKey, r.Job))));
        foreach (var v in r.Resources.Io.Distinct()) needs.Add(("io:" + v, 1));
        return needs;
    }

    void Pump()
    {
        var claimed = new Dictionary<string, int>(_held);
        var waitedDrives = new HashSet<string>();
        var order = _pending
            .OrderBy(r => Rank(_queueOf[r.Job]))
            .ThenBy(r => _queues[_queueOf[r.Job]].IndexOf(r.Job))
            .ThenBy(r => r.Order)
            .ToList();
        foreach (var r in order)
        {
            var queue = _queueOf[r.Job];
            var needs = Counted(r);
            var waitDrives = r.Resources.Drives.Distinct()
                .Where(d => (_driveHolder.TryGetValue(d, out var holder) && holder != r.Job)
                            || (waitedDrives.Contains(d) && !(_driveHolder.TryGetValue(d, out var h2) && h2 == r.Job)))
                .ToList();
            var waitKeys = needs.Where(n => claimed.GetValueOrDefault(n.Key) + n.Count > Capacity(n.Key, r.Job)).ToList();
            var gated = queue == Queue.Maintenance && _held.GetValueOrDefault(AcquisitionKey) > 0;
            var blocked = r.Resources.LibraryWrites.Select(l => _offline.GetValueOrDefault(l)).FirstOrDefault(e => e != null);
            if (blocked == null && !gated && waitDrives.Count == 0 && waitKeys.Count == 0)
            {
                Grant(r, needs);
                foreach (var (key, n) in needs) claimed[key] = claimed.GetValueOrDefault(key) + n;
                continue;
            }
            foreach (var d in waitDrives) waitedDrives.Add(d);
            foreach (var (key, n) in waitKeys) claimed[key] = claimed.GetValueOrDefault(key) + n;
            Wait wait = blocked != null ? new Wait.Blocked(blocked) : Wait.Waiting.Instance;
            if (wait != r.LastWait)
            {
                r.LastWait = wait;
                r.OnWait(wait);
            }
        }
        var snapshot = Leases();
        if (!snapshot.Equals(_published))
        {
            _published = snapshot;
            _bus.Publish(EngineEventType.LeaseChanged, LeaseSnapshot.Json(snapshot));
        }
    }

    void Grant(Request r, List<(string Key, int Count)> needs)
    {
        _pending.Remove(r);
        r.CancelRegistration?.Dispose();
        foreach (var d in r.Resources.Drives) _driveHolder[d] = r.Job;
        foreach (var (key, n) in needs) _held[key] = _held.GetValueOrDefault(key) + n;
        var lease = new Lease(r.Job, ++_granted, needs);
        if (!_leases.TryGetValue(r.Job, out var list)) _leases[r.Job] = list = new List<Lease>();
        list.Add(lease);
        r.Done.TrySetResult(lease);
    }

    void PublishQueue(Queue queue) =>
        _bus.Publish(EngineEventType.QueueChanged, QueueChange.Json(new QueueChange(queue, _queues[queue].ToList())));
}
