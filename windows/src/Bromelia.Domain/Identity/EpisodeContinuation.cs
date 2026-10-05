using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.RegularExpressions;

namespace Bromelia.Domain;

/// <summary>Where a TV disc's episode numbering continues: after the last episode of the previous disc of its
/// set (same show, season, part and volume; disc number one lower), or, in a media server library, after the
/// highest episode already in the season folder.</summary>
public static class EpisodeContinuation
{
    /// <summary>The previous disc's record when there is one; else the season folder's highest episode, unless a
    /// later disc of the set was archived (the discs were ripped out of order). Null for the first disc.</summary>
    public static PreviousEpisode? Choose(ContinuationQuery query, IReadOnlyList<ArchivedDisc> candidates)
    {
        if (query.Disc <= 1) return null;
        var same = candidates.Where(r => SameSet(r, query)).ToList();
        if (same.Where(r => r.Disc == query.Disc - 1 && r.LastEpisode != null).OrderByDescending(r => r.LastEpisode).FirstOrDefault() is { } prev)
            return new PreviousEpisode(prev.LastEpisode!.Value, prev.Folder);
        if (same.Any(r => (r.Disc ?? 0) > query.Disc) || query.SeasonFolderHighest is not { } last) return null;
        return new PreviousEpisode(last, query.SeasonFolder ?? "");
    }

    /// <summary>The highest episode number of <paramref name="season"/> in file names (<c>… S02E05 …</c>), or null.</summary>
    public static int? HighestInSeason(IEnumerable<string> fileNames, int season)
    {
        int? best = null;
        foreach (var n in fileNames.Where(n => !n.StartsWith(".")))
        {
            var m = SeasonEpisode.Match(n);
            if (m.Success && int.Parse(m.Groups[1].Value, CultureInfo.InvariantCulture) == season)
            {
                var e = int.Parse(m.Groups[2].Value, CultureInfo.InvariantCulture);
                if (best == null || e > best) best = e;
            }
        }
        return best;
    }

    private static readonly Regex SeasonEpisode = new(@"[Ss]([0-9]{1,3})[Ee]([0-9]{1,4})", RegexOptions.CultureInvariant);

    /// <summary>Whether an archived disc is of the query's set: the same show (its name or its label's title,
    /// normalised) and the same season, part and volume.</summary>
    public static bool SameSet(ArchivedDisc disc, ContinuationQuery query)
    {
        string name = Names.Normalize(query.Name), title = Names.Normalize(query.LabelTitle);
        var sameShow = (name.Length > 0 && Names.Normalize(disc.Name) == name) || (title.Length > 0 && Names.Normalize(disc.LabelTitle) == title);
        return sameShow && disc.Season == query.Season && disc.Part == query.Part && disc.Volume == query.Volume;
    }
}
