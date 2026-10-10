using System.Collections.Concurrent;
using Bromelia.Foundation;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Engine.Tests;

/// <summary>shared/fixtures/engine/engine-executor.cases.json: each case checked here.</summary>
public sealed class EngineExecutorTests
{
    static readonly TimeSpan Patience = TimeSpan.FromSeconds(10);

    static void Wait(Task task)
    {
        if (!task.Wait(Patience)) throw new TimeoutException("the executor didn't get there");
    }

    [Fact]
    public void TheSharedCasesPass()
    {
        RunCases("engine/engine-executor.cases.json", (id, given, expect) =>
        {
            var exec = new EngineExecutor("engine-test");
            try
            {
                switch (id)
                {
                    case "in-order":
                    {
                        var n = (int)given["actions"]!.AsInteger!;
                        var ran = new List<int>();
                        for (int i = 0; i < n; i++)
                        {
                            var k = i;
                            exec.Post(() => ran.Add(k));
                        }
                        Wait(exec.Barrier());
                        Assert.Equal(Enumerable.Range(0, n), ran);
                        return true;
                    }
                    case "in-order-per-thread":
                    {
                        var threads = (int)given["threads"]!.AsInteger!;
                        var each = (int)given["actionsEach"]!.AsInteger!;
                        var ran = new List<(int, int)>();
                        var posters = Enumerable.Range(0, threads).Select(t => new Thread(() =>
                        {
                            for (int i = 0; i < each; i++)
                            {
                                var k = i;
                                exec.Post(() => ran.Add((t, k)));
                            }
                        })).ToList();
                        posters.ForEach(p => p.Start());
                        posters.ForEach(p => p.Join());
                        Wait(exec.Barrier());
                        Same(expect["ran"]!.AsInteger, (long?)ran.Count, "ran");
                        for (int t = 0; t < threads; t++)
                            Assert.Equal(Enumerable.Range(0, each), ran.Where(r => r.Item1 == t).Select(r => r.Item2));
                        return true;
                    }
                    case "one-thread-of-its-own":
                    {
                        var threadIds = new ConcurrentDictionary<int, bool>();
                        var allCurrent = true;
                        for (int i = 0; i < (int)given["actions"]!.AsInteger!; i++)
                            exec.Post(() =>
                            {
                                threadIds[Thread.CurrentThread.ManagedThreadId] = true;
                                allCurrent &= exec.IsCurrent;
                            });
                        Wait(exec.Barrier());
                        Same(expect["isCurrentInActions"]!.AsBool, (bool?)allCurrent, "isCurrent in actions");
                        Same(expect["isCurrentOutside"]!.AsBool, (bool?)exec.IsCurrent, "isCurrent outside");
                        Same(expect["threads"]!.AsInteger, (long?)threadIds.Count, "threads");
                        Assert.DoesNotContain(Thread.CurrentThread.ManagedThreadId, threadIds.Keys);
                        return true;
                    }
                    case "posted-on-the-executor-runs-later":
                    {
                        var order = new List<string>();
                        using var busy = new ManualResetEventSlim();
                        exec.Post(() => busy.Wait(Patience));
                        exec.Post(() =>
                        {
                            order.Add("first");
                            exec.Post(() => order.Add("posted by first"));
                            order.Add("first ends");
                        });
                        exec.Post(() => order.Add("second"));
                        busy.Set();
                        Wait(exec.Barrier());
                        Wait(exec.Barrier());
                        Assert.Equal(expect["order"]!.AsArray!.Select(o => o.AsString), order);
                        return true;
                    }
                    case "worker-runs-off-the-executor":
                    {
                        bool? workOn = null, resultOn = null;
                        long result = 0;
                        var done = new TaskCompletionSource();
                        exec.Post(async () =>
                        {
                            result = await exec.Worker(() =>
                            {
                                workOn = exec.IsCurrent;
                                return 42L;
                            });
                            resultOn = exec.IsCurrent;
                            done.SetResult();
                        });
                        Wait(done.Task);
                        Same(expect["workOnExecutor"]!.AsBool, workOn, "work on the executor");
                        Same(expect["resultOnExecutor"]!.AsBool, resultOn, "result on the executor");
                        Same(expect["result"]!.AsInteger, (long?)result, "result");
                        return true;
                    }
                    case "worker-error":
                    {
                        var want = given["error"]!;
                        BroError? got = null;
                        bool? on = null;
                        var done = new TaskCompletionSource();
                        exec.Post(async () =>
                        {
                            try
                            {
                                await exec.Worker<int>(() => throw new BroFailure(new BroError(want["code"]!.AsString!, (JsonValue.Object)want["params"]!)));
                            }
                            catch (BroFailure f)
                            {
                                got = f.Error;
                                on = exec.IsCurrent;
                            }
                            done.SetResult();
                        });
                        Wait(done.Task);
                        Same(expect["errorOnExecutor"]!.AsBool, on, "error on the executor");
                        Same(expect["error"]!["code"]!.AsString, got?.Code, "code");
                        Same<JsonValue>(expect["error"]!["params"]!, got!.Params, "params");
                        return true;
                    }
                    case "workers-in-parallel":
                    {
                        var n = (int)given["workers"]!.AsInteger!;
                        using var all = new Barrier(n);
                        var tasks = new List<Task<bool>>();
                        var met = new ConcurrentBag<bool>();
                        var posted = new TaskCompletionSource();
                        exec.Post(() =>
                        {
                            for (int i = 0; i < n; i++)
                                tasks.Add(exec.Worker(() =>
                                {
                                    var r = all.SignalAndWait(Patience);
                                    met.Add(r);
                                    return r;
                                }));
                            posted.SetResult();
                        });
                        Wait(posted.Task);
                        Wait(Task.WhenAll(tasks));
                        Same(expect["allStartedBeforeAnyEnds"]!.AsBool, (bool?)(met.Count == n && met.All(m => m)), "all started before any ended");
                        return true;
                    }
                    case "barrier-waits":
                    {
                        var slowDone = false;
                        exec.Post(() =>
                        {
                            Thread.Sleep((int)given["slowActionMs"]!.AsInteger!);
                            slowDone = true;
                        });
                        Wait(exec.Barrier());
                        Same(expect["slowActionDoneFirst"]!.AsBool, (bool?)slowDone, "slow action done first");
                        return true;
                    }
                    case "stop-runs-what-is-queued":
                    {
                        var ran = 0;
                        using var hold = new ManualResetEventSlim();
                        exec.Post(() => hold.Wait(Patience));
                        for (int i = 0; i < (int)given["actions"]!.AsInteger!; i++) exec.Post(() => ran++);
                        var stopper = new Thread(exec.Stop);
                        stopper.Start();
                        while (exec.Post(() => { })) Thread.Sleep(1); // until the stop has begun
                        hold.Set();
                        stopper.Join();
                        var before = ran;
                        var accepted = exec.Post(() => ran++);
                        Thread.Sleep(20);
                        Same(expect["ranBeforeStop"]!.AsInteger, (long?)before, "ran before the stop ended");
                        Same(expect["postAfterStop"]!.AsBool, (bool?)accepted, "post after stop");
                        Same(expect["ranAfterStop"]!.AsInteger, (long?)(ran - before), "ran after stop");
                        return true;
                    }
                }
                return false;
            }
            finally
            {
                exec.Stop();
            }
        });
    }

    [Fact]
    public void AnUnexpectedExceptionGoesToItsHandlerAndTheExecutorGoesOn()
    {
        var seen = new List<Exception>();
        var exec = new EngineExecutor("engine-test", seen.Add);
        var after = false;
        exec.Post(() => throw new InvalidOperationException("a bug"));
        exec.Post(() => after = true);
        Wait(exec.Barrier());
        exec.Stop();
        Assert.Equal("a bug", Assert.Single(seen).Message);
        Assert.True(after);
    }

    [Fact]
    public void AwaitInEngineCodeComesBackToTheExecutor()
    {
        var exec = new EngineExecutor("engine-test");
        var on = new List<bool>();
        var done = new TaskCompletionSource();
        exec.Post(async () =>
        {
            await Task.Delay(10);
            on.Add(exec.IsCurrent);
            await Task.Yield();
            on.Add(exec.IsCurrent);
            done.SetResult();
        });
        Wait(done.Task);
        exec.Stop();
        Assert.Equal(new[] { true, true }, on);
    }
}
