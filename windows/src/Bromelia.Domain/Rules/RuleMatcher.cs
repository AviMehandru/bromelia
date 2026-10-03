using System;
using System.Linq;
using System.Text.RegularExpressions;

namespace Bromelia.Domain;

/// <summary>Whether a rule's conditions hold for a disc (was PluginMatcher).</summary>
public static class RuleMatcher
{
    /// <summary>Every condition given must hold; an empty <paramref name="when"/> matches every disc. The regular
    /// expressions are case-insensitive (an invalid one never matches); a format ending in * matches every code
    /// with that prefix (BR* = BR and BRe), others match exactly, ignoring case.</summary>
    public static bool Matches(RuleWhen when, RuleFacts facts)
    {
        if (when.NameOrLabel is { Length: > 0 } nl && !(Search(nl, facts.Name) || Search(nl, facts.Label))) return false;
        if (when.Name is { Length: > 0 } n && !Search(n, facts.Name)) return false;
        if (when.Label is { Length: > 0 } l && !Search(l, facts.Label)) return false;
        if (when.Formats is { } formats && formats.Count > 0 && !formats.Any(f => FormatMatches(f.Trim(), facts.FormatCode))) return false;
        if (when.Kinds is { } kinds && kinds.Count > 0 && !kinds.Contains(facts.Kind ?? "")) return false;
        if (when.Drives is { } drives && drives.Count > 0 && !drives.Contains(facts.DriveId ?? "")) return false;
        if (when.Profiles is { } profiles && profiles.Count > 0 && !profiles.Contains(facts.ProfileId ?? "")) return false;
        if (when.Automatic is { } automatic && automatic != facts.Automatic) return false;
        return true;
    }

    private static bool Search(string pattern, string text)
    {
        try { return Regex.IsMatch(text, pattern, RegexOptions.IgnoreCase | RegexOptions.CultureInvariant); }
        catch (ArgumentException) { return false; }
    }

    private static bool FormatMatches(string format, string code) => format.EndsWith('*')
        ? MessageCatalog.AsciiLower(code).StartsWith(MessageCatalog.AsciiLower(format.Substring(0, format.Length - 1)), StringComparison.Ordinal)
        : MessageCatalog.AsciiLower(format) == MessageCatalog.AsciiLower(code);
}
