namespace Bromelia.Domain;

/// <summary>What rules are matched against: the movie or show name, the disc label, the format code, movie or TV,
/// the drive entry's id, the profile the disc got before rules, and whether the rip is automatic.</summary>
public sealed record RuleFacts(
    string Name,
    string Label,
    string FormatCode,
    string? Kind = null,
    string? DriveId = null,
    string? ProfileId = null,
    bool Automatic = false);
