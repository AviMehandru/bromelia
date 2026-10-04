using System;
using System.Collections.Generic;
using System.Linq;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Adapters;

/// <summary>A DeviceMonitor that looks at the drives every interval and reports what changed
/// (OsDriveState.Changes; shared/fixtures/adapters/os-drive-states.cases.json). The platform monitors are this over the
/// system's snapshot.</summary>
public sealed class DrivePoller : IDeviceMonitor
{
    readonly Func<IReadOnlyList<OsDriveState>> _snapshot;
    readonly IClock _clock;
    readonly Duration _interval;
    readonly object _gate = new();
    IReadOnlyList<OsDriveState> _last = Array.Empty<OsDriveState>();
    ITimerHandle? _timer;
    Action<DeviceEvent>? _sink;

    public DrivePoller(Func<IReadOnlyList<OsDriveState>> snapshot, IClock clock, Duration interval)
    {
        _snapshot = snapshot;
        _clock = clock;
        _interval = interval;
    }

    /// <summary>Takes a snapshot now (no events for what is already there), then one each interval.</summary>
    public void Start(Action<DeviceEvent> sink)
    {
        lock (_gate)
        {
            if (_timer is not null) return;
            _sink = sink;
            _last = _snapshot();
            _timer = _clock.Timer(new TimerSchedule.Every(_interval), Look);
        }
    }

    public void Stop()
    {
        lock (_gate)
        {
            _timer?.Cancel();
            _timer = null;
            _sink = null;
        }
    }

    public IReadOnlyList<OsDrive> CurrentDrives() => _snapshot().Select(s => s.Drive).ToList();

    /// <summary>One look; the events go to the sink outside the lock (the sink may stop the poller).</summary>
    void Look()
    {
        Action<DeviceEvent> sink;
        IReadOnlyList<DeviceEvent> events;
        lock (_gate)
        {
            if (_sink is null) return;
            sink = _sink;
            var now = _snapshot();
            events = OsDriveState.Changes(_last, now);
            _last = now;
        }
        foreach (var e in events) sink(e);
    }
}
