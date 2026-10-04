namespace Bromelia.Domain;

/// <summary>MakeMKV's free beta key, replaced about once a month.</summary>
public static class BetaKey
{
    /// <summary>Whether an automatic update may replace <paramref name="currentKey"/>: only a beta key (T-…) or no
    /// key, never a purchased one.</summary>
    public static bool MayReplace(string? currentKey) => string.IsNullOrWhiteSpace(currentKey) || currentKey.Trim().StartsWith("T-", System.StringComparison.Ordinal);
}
