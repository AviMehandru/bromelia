using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>Optical drives as the operating system sees them.</summary>
public interface IDeviceMonitor
{
    /// <summary>Reports events to sink (on the monitor's thread) until stop.</summary>
    void Start(Action<DeviceEvent> sink);

    void Stop();

    IReadOnlyList<OsDrive> CurrentDrives();
}
