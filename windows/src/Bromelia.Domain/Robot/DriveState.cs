namespace Bromelia.Domain;

/// <summary>A drive's state in a DRV line; MakeMKV's numbers are 0, 1, 2, 3, 256 and 257.</summary>
public enum DriveState
{
    EmptyClosed,
    EmptyOpen,
    Inserted,
    Loading,
    NoDrive,
    Unmounting,
}
