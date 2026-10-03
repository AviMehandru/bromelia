using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;

namespace Bromelia.Domain;

/// <summary>Orders search results.</summary>
public static class Ranking
{
    /// <summary>Lower case, letters and digits only: "Dune: Part Two" → "duneparttwo".</summary>
    internal static string Normalize(string s)
    {
        var sb = new StringBuilder();
        foreach (var r in s.EnumerateRunes())
            if (Rune.IsLetterOrDigit(r)) sb.Append(Rune.ToLowerInvariant(r).ToString());
        return sb.ToString();
    }

    /// <summary>Best first: the title matching <paramref name="name"/> (ignoring case and punctuation) scores 4, the
    /// year 2 (1 when it is off by one); ties keep the provider's order.</summary>
    public static List<Candidate> Rank(IReadOnlyList<Candidate> candidates, string name, int? year)
    {
        var want = Normalize(name);
        int Score(Candidate c) => (Normalize(c.Title) == want ? 4 : 0)
            + (year is { } y && c.Year is { } cy ? (cy == y ? 2 : Math.Abs(cy - y) == 1 ? 1 : 0) : 0);
        return candidates.Select((c, i) => (c, i)).OrderByDescending(x => Score(x.c)).ThenBy(x => x.i).Select(x => x.c).ToList();
    }
}
