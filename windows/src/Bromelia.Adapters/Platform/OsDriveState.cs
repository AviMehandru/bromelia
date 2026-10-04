using System.Collections.Generic;
using System.Linq;
using Bromelia.Domain;
using Bromelia.Ports;

namespace Bromelia.Adapters;

/// <summary>An optical drive as the operating system sees it at one moment, and whether it holds media
/// (shared/fixtures/adapters/os-drive-states.cases.json).</summary>
public sealed record OsDriveState(OsDrive Drive, bool Media)
{
    /// <summary>The events between two snapshots: for each drive of <paramref name="after"/>, appeared or what changed
    /// (unmounted, media removed, media arrived, mounted); then each drive that is gone (unmounted, media removed,
    /// vanished).</summary>
    public static IReadOnlyList<DeviceEvent> Changes(IReadOnlyList<OsDriveState> before, IReadOnlyList<OsDriveState> after)
    {
        var events = new List<DeviceEvent>();
        foreach (var now in after)
        {
            var device = now.Drive.Device;
            var was = before.FirstOrDefault(b => b.Drive.Device == device);
            if (was is null) events.Add(new DeviceEvent.DriveAppeared(now.Drive));
            else
            {
                if (was.Drive.MountPath is not null && was.Drive.MountPath != now.Drive.MountPath) events.Add(new DeviceEvent.Unmounted(device));
                if (was.Media && !now.Media) events.Add(new DeviceEvent.MediaRemoved(device));
            }
            if (now.Media && was?.Media != true) events.Add(new DeviceEvent.MediaArrived(device));
            if (now.Drive.MountPath is { } path && path != was?.Drive.MountPath) events.Add(new DeviceEvent.Mounted(device, path));
        }
        foreach (var gone in before.Where(b => after.All(a => a.Drive.Device != b.Drive.Device)))
        {
            if (gone.Drive.MountPath is not null) events.Add(new DeviceEvent.Unmounted(gone.Drive.Device));
            if (gone.Media) events.Add(new DeviceEvent.MediaRemoved(gone.Drive.Device));
            events.Add(new DeviceEvent.DriveVanished(gone.Drive.Device));
        }
        return events;
    }
}
