using System.Collections.Generic;

namespace Bromelia.Domain;

/// <summary>A rule's conditions; every condition given must hold, and an empty one matches every disc:
/// regular expressions on the name and label, format codes (a trailing * matches a prefix), kinds, drive
/// entries, the profile before rules, and automatic or not.</summary>
public sealed record RuleWhen(
    string? NameOrLabel = null,
    string? Name = null,
    string? Label = null,
    IReadOnlyList<string>? Formats = null,
    IReadOnlyList<string>? Kinds = null,
    IReadOnlyList<string>? Drives = null,
    IReadOnlyList<string>? Profiles = null,
    bool? Automatic = null);
