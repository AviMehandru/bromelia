namespace Bromelia.Domain;

/// <summary>What <see cref="TokenRegistry.Values"/> needs besides the identity: the rip word (Rip / Backup), the
/// drive's name, the disc and volume names, the disc type, the job's short id, the release year (from the online
/// lookup) and the local time.</summary>
public sealed record TokenContext(
    string Rip,
    string Drive = "",
    string Disc = "",
    string Volume = "",
    DiscType Type = DiscType.Disc,
    string Job = "",
    int? ReleaseYear = null,
    LocalTime? LocalTime = null);
