using Bromelia.Foundation;
using Bromelia.Ports;
using Xunit;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Adapters.Tests;

public class SystemClockTests
{
    readonly SystemClock _clock = new();

    [Fact]
    public void NowIsTheWallClock()
    {
        var now = _clock.Now().UnixMilliseconds;
        Assert.InRange(now, DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() - 1000, DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() + 1000);
    }

    [Fact]
    public async Task SleepsAndTheMonotonicClockAdvances()
    {
        var before = _clock.Monotonic();
        await _clock.Sleep(new Duration(0.2), new CancellationSource().Token);
        var slept = _clock.Monotonic().Seconds - before.Seconds;
        Assert.InRange(slept, 0.15, 5);
    }

    [Fact]
    public async Task ACancelledSleepFailsWithJobCancelled()
    {
        var cancelSource = new CancellationSource();
        var cancel = cancelSource.Token;
        var sleep = _clock.Sleep(new Duration(30), cancel);
        cancelSource.Cancel();
        var e = await Assert.ThrowsAsync<BroFailure>(() => sleep);
        Assert.Equal("job.cancelled", e.Error.Code);
    }

    [Fact]
    public void TimersFireOnceOrRepeatedlyUntilCancelled()
    {
        using var once = new SemaphoreSlim(0);
        _clock.Timer(new TimerSchedule.At(new Instant(_clock.Now().UnixMilliseconds + 100)), () => once.Release());
        Assert.True(once.Wait(TimeSpan.FromSeconds(5)));

        var count = 0;
        var every = _clock.Timer(new TimerSchedule.Every(new Duration(0.05)), () => Interlocked.Increment(ref count));
        var deadline = DateTime.UtcNow.AddSeconds(5);
        while (Volatile.Read(ref count) < 3 && DateTime.UtcNow < deadline) Thread.Sleep(10);
        every.Cancel();
        Assert.True(Volatile.Read(ref count) >= 3);
        Thread.Sleep(100);
        var after = Volatile.Read(ref count);
        Thread.Sleep(200);
        Assert.Equal(after, Volatile.Read(ref count));
    }
}
