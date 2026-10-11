using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;

namespace Bromelia.Engine;

/// <summary>
/// The one thread that owns all service state (plan §8). Actions posted from any thread run one at a time, in the
/// order they were posted, never inside <see cref="Post"/>. The thread's SynchronizationContext is the executor's, so
/// an <c>await</c> in engine code comes back here. Blocking work goes to <see cref="Worker{T}(Func{T})"/>, whose result
/// is delivered back on the executor. Today's daemon <c>Loop</c> and <c>UiDispatcher</c>, as one type.
/// </summary>
public sealed class EngineExecutor
{
    readonly object _lock = new();
    readonly Queue<Action> _queue = new();
    readonly Thread _thread;
    readonly Action<Exception>? _unexpected;
    bool _stopping;
    int _workers; // started, result not yet posted back

    /// <param name="unexpected">Gets an exception an action let out (a bug); the executor goes on. Without it the
    /// exception ends the process, as an unhandled exception does.</param>
    public EngineExecutor(string name = "bromelia-engine", Action<Exception>? unexpected = null)
    {
        _unexpected = unexpected;
        _thread = new Thread(Run) { Name = name, IsBackground = true };
        _thread.Start();
    }

    /// <summary>True on the executor's thread.</summary>
    public bool IsCurrent => Thread.CurrentThread == _thread;

    /// <summary>Work not done yet: actions waiting to run (not the one running) and workers whose result hasn't been
    /// posted back. A test (or a scenario runner) knows the engine has settled when this is down to the workers it
    /// knows are parked (a step waiting for an answer).</summary>
    public int Activity
    {
        get
        {
            lock (_lock) return _queue.Count + _workers;
        }
    }

    /// <summary>Runs <paramref name="action"/> on the executor after everything posted before it. False, and the
    /// action never runs, once the executor is stopping.</summary>
    public bool Post(Action action)
    {
        lock (_lock)
        {
            if (_stopping) return false;
            _queue.Enqueue(action);
            Monitor.Pulse(_lock);
        }
        return true;
    }

    /// <summary>Runs <paramref name="work"/> on a worker thread; its result, or what it threw, comes back on the
    /// executor (awaiting it there resumes there).</summary>
    public Task<T> Worker<T>(Func<T> work)
    {
        var done = new TaskCompletionSource<T>(TaskCreationOptions.RunContinuationsAsynchronously);
        lock (_lock) _workers++;
        Task.Run(() =>
        {
            try
            {
                var result = work();
                Deliver(() => done.SetResult(result));
            }
            catch (Exception e)
            {
                Deliver(() => done.SetException(e));
            }
        });
        return done.Task;
    }

    /// <summary>The same for work that is asynchronous itself (a step, a store call): it starts on a worker thread,
    /// never on the executor.</summary>
    public Task<T> Worker<T>(Func<Task<T>> work)
    {
        var done = new TaskCompletionSource<T>(TaskCreationOptions.RunContinuationsAsynchronously);
        lock (_lock) _workers++;
        Task.Run(async () =>
        {
            try
            {
                var result = await work().ConfigureAwait(false);
                Deliver(() => done.SetResult(result));
            }
            catch (Exception e)
            {
                Deliver(() => done.SetException(e));
            }
        });
        return done.Task;
    }

    /// <summary>The same for asynchronous work without a result.</summary>
    public Task Worker(Func<Task> work) => Worker(async () =>
    {
        await work().ConfigureAwait(false);
        return true;
    });

    /// <summary>Posts a worker's result: from then on it counts as a queued action instead of a worker.</summary>
    void Deliver(Action action)
    {
        lock (_lock)
        {
            _workers--;
            if (_stopping) return;
            _queue.Enqueue(action);
            Monitor.Pulse(_lock);
        }
    }

    /// <summary>Completes once every action posted before it has run (at once when stopped). Waiting for it
    /// synchronously on the executor would never end.</summary>
    public Task Barrier()
    {
        var done = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        if (!Post(() => done.SetResult())) done.SetResult();
        return done.Task;
    }

    /// <summary>Lets the queued actions run, then ends the thread; later posts don't run. Waits for the end, unless
    /// called on the executor (it then ends after the current action and what is queued).</summary>
    public void Stop()
    {
        lock (_lock)
        {
            _stopping = true;
            Monitor.Pulse(_lock);
        }
        if (!IsCurrent) _thread.Join();
    }

    void Run()
    {
        SynchronizationContext.SetSynchronizationContext(new Context(this));
        while (true)
        {
            Action action;
            lock (_lock)
            {
                while (_queue.Count == 0 && !_stopping) Monitor.Wait(_lock);
                if (_queue.Count == 0) return;
                action = _queue.Dequeue();
            }
            try
            {
                action();
            }
            catch (Exception e) when (_unexpected != null)
            {
                _unexpected(e);
            }
        }
    }

    /// <summary>Brings <c>await</c> continuations back onto the executor.</summary>
    sealed class Context : SynchronizationContext
    {
        readonly EngineExecutor _executor;

        public Context(EngineExecutor executor) { _executor = executor; }

        public override void Post(SendOrPostCallback d, object? state) => _executor.Post(() => d(state));

        public override void Send(SendOrPostCallback d, object? state)
        {
            if (!_executor.IsCurrent) throw new InvalidOperationException("Send to the engine executor from another thread");
            d(state);
        }

        public override SynchronizationContext CreateCopy() => this;
    }
}
