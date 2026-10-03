using System.Collections.Generic;
using System.Linq;

namespace Bromelia.Domain;

/// <summary>Ripping chosen titles in one makemkvcon run. <c>mkv</c> takes one title or <c>all</c>, so a subset
/// can only be ripped in one run when a minimum title length leaves exactly that subset.</summary>
public static class OnePass
{
    /// <summary>Rips with fewer titles gain nothing: the extra listing costs as much as the runs it saves.</summary>
    private const int MinimumTitles = 3;

    /// <summary>The minimum length that keeps the chosen titles and drops the others, or null (rip title by
    /// title). Listed lengths are rounded to seconds, so the longest title left out must be at least 2 s shorter
    /// than the shortest chosen; the length must also be longer than the one already in use.</summary>
    public static OnePassPlan? Plan(IReadOnlyCollection<int> indices, Listing listing, int? currentMinLength)
    {
        var set = indices.ToHashSet();
        if (set.Count < MinimumTitles || set.Count >= listing.Titles.Count) return null;
        var picked = listing.Titles.Where(t => set.Contains(t.Index)).Select(t => t.DurationSeconds).ToList();
        var others = listing.Titles.Where(t => !set.Contains(t.Index)).Select(t => t.DurationSeconds).ToList();
        if (picked.Count != set.Count || others.Count == 0 || picked.Min() - others.Max() < 2) return null;
        int length = others.Max() + 1;
        return length > (currentMinLength ?? 0) ? new OnePassPlan(length) : null;
    }

    /// <summary>Whether <paramref name="listing"/> (read with that minimum length) holds exactly the chosen titles
    /// of <paramref name="original"/>.</summary>
    public static bool Matches(Listing listing, IReadOnlyCollection<int> chosen, Listing original)
    {
        var set = chosen.ToHashSet();
        if (listing.Titles.Count != set.Count) return false;
        try
        {
            var map = ListingMatch.MapTitles(set, original, listing, new HashSet<int>());
            return map.Values.ToHashSet().SetEquals(listing.Titles.Select(t => t.Index));
        }
        catch (Foundation.BroFailure)
        {
            return false;
        }
    }
}
