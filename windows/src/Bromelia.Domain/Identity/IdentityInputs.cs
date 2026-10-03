namespace Bromelia.Domain;

/// <summary>What <see cref="Identity.Resolve"/> works from: the listing (when there is one), the disc's label,
/// whether a backup stays encrypted, the drive's flags, a format already known (a backup's structure), the
/// user's choices, and how many episodes the menu plays in one title.</summary>
public sealed record IdentityInputs(
    Listing? Listing,
    string DiscLabel,
    bool Encrypted,
    DiscFlags? Flags = null,
    DiscFormat? Format = null,
    string NameOverride = "",
    MediaKind? KindOverride = null,
    int PlayAllEpisodes = 0);
