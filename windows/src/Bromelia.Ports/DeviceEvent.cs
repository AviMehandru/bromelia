using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>What the operating system reported about a drive.</summary>
public abstract record DeviceEvent
{
    public sealed record DriveAppeared(OsDrive Drive) : DeviceEvent;
    public sealed record DriveVanished(string Device) : DeviceEvent;
    public sealed record MediaArrived(string Device) : DeviceEvent;
    public sealed record MediaRemoved(string Device) : DeviceEvent;
    public sealed record TrayOpened(string Device) : DeviceEvent;
    public sealed record Mounted(string Device, string Path) : DeviceEvent;
    public sealed record Unmounted(string Device) : DeviceEvent;
}
