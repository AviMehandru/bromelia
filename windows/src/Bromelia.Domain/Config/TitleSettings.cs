namespace Bromelia.Domain;

/// <summary>A profile's title rules (config-3.json's <c>TitleRules</c> object; <see cref="TitleRules"/> applies
/// them): filters (duration, chapters, size, patterns, angles), duplicate removal, the strategy, then maxTitles.
/// The defaults are config-3.json's.</summary>
public sealed record TitleSettings(
    TitleStrategy Strategy = TitleStrategy.All,
    int LongestCount = 1,
    string IndexPattern = "",
    IndexBase IndexBase = IndexBase.Makemkv,
    int MinDurationSeconds = 0,
    int MaxDurationSeconds = 0,
    int MinChapters = 0,
    int MaxChapters = 0,
    int MinSizeMB = 0,
    int MaxSizeMB = 0,
    string IncludePattern = "",
    string ExcludePattern = "",
    bool SkipDuplicates = true,
    bool SkipAlternateAngles = false,
    int MaxTitles = 0);
