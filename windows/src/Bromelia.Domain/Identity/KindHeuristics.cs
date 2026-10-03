using System;
using System.Collections.Generic;
using System.Linq;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>Whether a disc is a movie or a TV show.</summary>
public static class KindHeuristics
{
    /// <summary>TV when the label has season / volume markers, the menu plays three or more episodes in one title,
    /// or three or more titles are of episode length; a movie otherwise.</summary>
    public static Decision Decide(Label label, Listing? listing, int playAllEpisodes)
    {
        if (label.LooksLikeSeries) return new(MediaKind.Tv, new BroMessage(MessageCode.IdentityReasonLabelMarkers));
        if (playAllEpisodes >= 3)
            return new(MediaKind.Tv, new BroMessage(MessageCode.IdentityReasonMenuEpisodes, Severity.Info, ("count", JsonValue.Of(playAllEpisodes))));
        if (EpisodeLike(listing?.Titles ?? new List<Title>()).Count >= 3)
            return new(MediaKind.Tv, new BroMessage(MessageCode.IdentityReasonEpisodeTitles));
        return new(MediaKind.Movie, new BroMessage(MessageCode.IdentityReasonNone));
    }

    /// <summary>Titles of 10–75 minutes within ±35 % of the median of such titles (none when fewer than two).</summary>
    public static List<Title> EpisodeLike(IReadOnlyList<Title> titles)
    {
        var candidates = titles.Where(t => t.DurationSeconds is >= 600 and <= 4500).ToList();
        if (candidates.Count < 2) return new List<Title>();
        var sorted = candidates.Select(t => t.DurationSeconds).OrderBy(d => d).ToList();
        double median = sorted[sorted.Count / 2];
        return candidates.Where(t => Math.Abs(t.DurationSeconds - median) <= median * 0.35).ToList();
    }
}
