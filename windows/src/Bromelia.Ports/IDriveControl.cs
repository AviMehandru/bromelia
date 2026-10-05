using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>Trays, mounts and raw access.</summary>
public interface IDriveControl
{
    Task Eject(string device);

    Task CloseTray(string device);

    /// <summary>The mount path; none after timeout or when cancelled.</summary>
    Task<string?> WaitForMount(string device, Duration timeout, CancellationToken cancel);

    DiscContent ProbeContent(string device);

    /// <summary>Whole 2048-byte sectors of a drive's disc or an image file; fails when the size can't be read (drive.sizeUnknown) or
    /// isn't a whole number of sectors (drive.partialSector).</summary>
    ISectorReader OpenRaw(string device);
}
