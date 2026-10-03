using Bromelia.Foundation;

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
    int MaxTitles = 0)
{
    /// <summary>Title rules from their JSON (config-3.json's TitleRules), with defaults for what it leaves out.</summary>
    public static TitleSettings Decode(JsonValue json)
    {
        var j = SchemaWalker.Normalize(json, SchemaWalker.Def("TitleRules"), "", true, new System.Collections.Generic.List<Issue>());
        int I(string k) => (int)(j[k]?.AsInteger ?? 0);
        return new TitleSettings(EnumWire.Parse<TitleStrategy>(j["strategy"]?.AsString) ?? TitleStrategy.All, I("longestCount"),
            j["indexPattern"]?.AsString ?? "", EnumWire.Parse<IndexBase>(j["indexBase"]?.AsString) ?? IndexBase.Makemkv,
            I("minDurationSeconds"), I("maxDurationSeconds"), I("minChapters"), I("maxChapters"), I("minSizeMB"), I("maxSizeMB"),
            j["includePattern"]?.AsString ?? "", j["excludePattern"]?.AsString ?? "", j["skipDuplicates"]?.AsBool ?? true,
            j["skipAlternateAngles"]?.AsBool ?? false, I("maxTitles"));
    }
}
