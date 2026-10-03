namespace Bromelia.Domain;

/// <summary>How a drive entry finds its drive: <see cref="DriveName"/> (MakeMKV's identification, compared
/// case- and space-insensitively), else <see cref="DevicePath"/>.</summary>
public sealed record DriveMatch(string DriveName = "", string DevicePath = "");
