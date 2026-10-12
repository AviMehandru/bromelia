namespace Bromelia.Engine;

/// <summary>Where a drive is now (implemented by DriveService): MakeMKV numbers drives by scan order and the device
/// can change, so a step asks when it runs.</summary>
public interface IDriveLookup
{
    /// <summary>Null when no such drive is connected.</summary>
    DriveLocation? Find(string driveId);
}
