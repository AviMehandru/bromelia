using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.TestKit;

/// <summary>The operating system's drives, faked (plan §17): what <see cref="CurrentDrives"/> lists, the events
/// <see cref="Send"/> reports to the monitor's sink, the content <see cref="ProbeContent"/> finds for a device (video unless
/// <see cref="SetContent"/> says otherwise),
/// and the tray calls, recorded in <see cref="Calls"/> ("eject &lt;device&gt;", "closeTray &lt;device&gt;"). A mount wait
/// answers the drive's mount path at once. Raw sector reads aren't faked. <see cref="OnEject"/> and <see cref="OnCloseTray"/>
/// hear of each tray call after it is recorded (a scenario's drive world changes with it). Thread-safe.</summary>
public sealed class FakeDrives : IDeviceMonitor, IDriveControl
{
    readonly object _lock = new();
    readonly List<OsDrive> _drives = new();
    readonly Dictionary<string, DiscContent> _contents = new();
    readonly List<string> _calls = new();
    Action<DeviceEvent>? _sink;
    /// <summary>Hears of each eject the engine asks for, with its device (outside the lock).</summary>
    public Action<string>? OnEject;
    /// <summary>Hears of each tray close the engine asks for, with its device (outside the lock).</summary>
    public Action<string>? OnCloseTray;

    /// <summary>A drive the OS lists from now on.</summary>
    public void Add(OsDrive drive)
    {
        lock (_lock)
        {
            _drives.RemoveAll(d => d.Device == drive.Device);
            _drives.Add(drive);
        }
    }

    /// <summary>What <see cref="ProbeContent"/> finds on the device (the one the engine names: MakeMKV's).</summary>
    public void SetContent(string device, DiscContent content)
    {
        lock (_lock) _contents[device] = content;
    }

    /// <summary>Reports the event to the monitor's sink (nothing before start or after stop).</summary>
    public void Send(DeviceEvent e)
    {
        Action<DeviceEvent>? sink;
        lock (_lock) sink = _sink;
        sink?.Invoke(e);
    }

    /// <summary>Whether the monitor is started (a sink is kept).</summary>
    public bool Monitoring()
    {
        lock (_lock) return _sink != null;
    }

    public IReadOnlyList<string> Calls()
    {
        lock (_lock) return _calls.ToList();
    }

    public void Start(Action<DeviceEvent> sink)
    {
        lock (_lock) _sink = sink;
    }

    public void Stop()
    {
        lock (_lock) _sink = null;
    }

    public IReadOnlyList<OsDrive> CurrentDrives()
    {
        lock (_lock) return _drives.ToList();
    }

    public Task Eject(string device)
    {
        lock (_lock) _calls.Add("eject " + device);
        OnEject?.Invoke(device);
        return Task.CompletedTask;
    }

    public Task CloseTray(string device)
    {
        lock (_lock) _calls.Add("closeTray " + device);
        OnCloseTray?.Invoke(device);
        return Task.CompletedTask;
    }

    public Task<string?> WaitForMount(string device, Duration timeout, CancellationToken cancel)
    {
        lock (_lock) return Task.FromResult(_drives.FirstOrDefault(d => d.Device == device)?.MountPath);
    }

    public DiscContent ProbeContent(string device)
    {
        lock (_lock) return _contents.GetValueOrDefault(device, DiscContent.Video);
    }

    public ISectorReader OpenRaw(string device) => throw new NotSupportedException("raw sector reads aren't faked");
}
