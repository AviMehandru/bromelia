using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.RegularExpressions;

namespace Bromelia.Domain;

/// <summary>Episode numbers read (by OCR) from a DVD's menus.</summary>
public static class MenuNumbers
{
    private static readonly Regex EpisodeText = new(@"EPIS[O0]DE\s*#?\s*([0-9]{1,4})\b", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);

    /// <summary>The numbers after "Episode" (OCR may read the O as 0), in the order they appear, each once.</summary>
    public static List<int> Parse(string text) =>
        EpisodeText.Matches(text).Select(m => int.Parse(m.Groups[1].Value, CultureInfo.InvariantCulture)).Distinct().ToList();

    /// <summary>The first episode S such that [S, S+count) covers the most numbers; null without a unique best
    /// choice backed by at least two numbers.</summary>
    public static int? FirstEpisode(IReadOnlyCollection<int> numbers, int count)
    {
        var set = numbers.ToHashSet();
        if (set.Count == 0 || count <= 0) return null;
        var scores = new Dictionary<int, int>();
        foreach (var n in set)
            for (int k = 0; k < count; k++)
                if (n - k >= 0) scores[n - k] = 0;
        foreach (var s in scores.Keys.ToList()) scores[s] = set.Count(n => s <= n && n < s + count);
        int best = scores.Values.Max();
        var winners = scores.Where(kv => kv.Value == best).Select(kv => kv.Key).ToList();
        return winners.Count == 1 && best >= 2 ? winners[0] : null;
    }
}
