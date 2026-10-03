using System;
using System.Collections.Generic;
using System.Linq;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>How much space a rip needs (listing sizes are estimates).</summary>
public static class SpaceEstimate
{
    /// <summary><paramref name="bytes"/> plus a margin: 2 % or 256 MiB, whichever is larger.</summary>
    public static Bytes Required(Bytes bytes) => new(bytes.Count + Math.Max(256L << 20, bytes.Count / 50));

    /// <summary>The chosen titles, plus a second copy of the titles with hand-picked tracks (remuxed) and of the
    /// "play all" title (split into episodes).</summary>
    public static Bytes ForPlan(IReadOnlyList<Title> titles, IReadOnlyCollection<int> handPicked, int? splitTitle)
    {
        long need = titles.Sum(t => t.SizeBytes) + titles.Where(t => handPicked.Contains(t.Index)).Sum(t => t.SizeBytes);
        if (splitTitle is { } s && titles.FirstOrDefault(t => t.Index == s) is { } playAll) need += playAll.SizeBytes;
        return new Bytes(need);
    }
}
