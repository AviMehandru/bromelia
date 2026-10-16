namespace Bromelia.Engine;

/// <summary>A drive's state as clients see it (dtos.json Drive.state): MakeMKV's state of a connected drive, or
/// unavailable for a configured drive that isn't connected.</summary>
public enum DriveStatus
{
    Empty,
    TrayOpen,
    Loading,
    Present,
    Unavailable,
    Unmounting,
}
