namespace Bromelia.Domain;

/// <summary>Whether the drive read the disc in LibreDrive mode, didn't, or the disc needs it.</summary>
public enum LibreDriveState
{
    Enabled,
    NotInUse,
    Required,
}
