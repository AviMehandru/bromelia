using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Xunit;
using static Bromelia.Tests.Fixtures;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Adapters.Tests;

/// <summary>A clock whose timers fire when the test says so.</summary>
internal sealed class ManualClock : IClock
{
    public readonly List<(TimerSchedule Schedule, Action Handler, Handle Timer)> Timers = new();
    public Instant Now() => new(1_790_000_000_000);
    public Duration Monotonic() => new(0);
    public Task Sleep(Duration duration, CancellationToken cancel) => Task.CompletedTask;

    public ITimerHandle Timer(TimerSchedule schedule, Action handler)
    {
        var h = new Handle();
        Timers.Add((schedule, handler, h));
        return h;
    }

    /// <summary>Fires every timer that isn't cancelled.</summary>
    public void Tick()
    {
        foreach (var t in Timers.ToList())
            if (!t.Timer.Cancelled) t.Handler();
    }

    internal sealed class Handle : ITimerHandle
    {
        public bool Cancelled;
        public void Cancel() => Cancelled = true;
    }
}

/// <summary>shared/fixtures/adapters/os-drive-states.cases.json.</summary>
public sealed class OsDriveStateTests
{
    static OsDriveState State(JsonValue j) =>
        new(new OsDrive(j["device"]!.AsString!, j["identification"]?.AsString ?? "", j["mountPath"]?.AsString), j["media"]?.AsBool == true);

    static List<OsDriveState> States(JsonValue j) => j.AsArray!.Select(State).ToList();

    static string Text(DeviceEvent e) => e switch
    {
        DeviceEvent.DriveAppeared a => $"driveAppeared {a.Drive.Device} {a.Drive.Identification}",
        DeviceEvent.DriveVanished v => $"driveVanished {v.Device}",
        DeviceEvent.MediaArrived m => $"mediaArrived {m.Device}",
        DeviceEvent.MediaRemoved m => $"mediaRemoved {m.Device}",
        DeviceEvent.TrayOpened t => $"trayOpened {t.Device}",
        DeviceEvent.Mounted m => $"mounted {m.Device} {m.Path}",
        DeviceEvent.Unmounted u => $"unmounted {u.Device}",
        _ => e.ToString(),
    };

    [Fact]
    public void TheSharedCasesPass()
    {
        RunCases("adapters/os-drive-states.cases.json", (id, given, expect) =>
        {
            if (given["snapshots"] is { } snapshots)
            {
                var all = snapshots.AsArray!.Select(States).ToList();
                var index = 0;
                var clock = new ManualClock();
                var poller = new DrivePoller(() => all[index], clock, new Duration(given["interval"]!.AsNumber!.Value));
                var got = new List<string>();
                poller.Start(e => got.Add(Text(e)));
                var (schedule, _, timer) = Assert.Single(clock.Timers);
                Assert.Equal(new TimerSchedule.Every(new Duration(expect["timer"]!.AsNumber!.Value)), schedule);
                foreach (var want in expect["afterTicks"]!.AsArray!)
                {
                    index++;
                    got.Clear();
                    clock.Tick();
                    Assert.Equal(want.AsArray!.Select(w => w.AsString!), got);
                }
                Assert.Equal(all[index].Select(s => s.Drive), poller.CurrentDrives());
                poller.Stop();
                Assert.True(timer.Cancelled);
                return true;
            }
            Assert.Equal(expect["events"]!.AsArray!.Select(e => e.AsString!), OsDriveState.Changes(States(given["before"]!), States(given["after"]!)).Select(Text));
            return true;
        });
    }
}
