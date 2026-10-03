using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>Matches the titles of one listing to another: the listing a disc was opened with and the listing read
/// when the job starts, or a disc and its backup. MakeMKV numbers titles by position, which changes with the
/// minimum length setting, so titles are matched by source title, duration and segment map.</summary>
public static class ListingMatch
{
    /// <summary>Why <paramref name="new"/> looks like another disc than <paramref name="old"/>
    /// (disc.changed.volume / disc.changed.titles), or null when it may be the same disc.</summary>
    public static BroMessage? DifferentDisc(Listing old, Listing @new)
    {
        if (old.VolumeName.Length > 0 && @new.VolumeName.Length > 0 && old.VolumeName != @new.VolumeName)
            return new BroMessage(MessageCode.DiscChangedVolume, Severity.Info, ("old", JsonValue.Of(old.VolumeName)), ("new", JsonValue.Of(@new.VolumeName)));
        var a = old.Titles.Select(Key).ToHashSet();
        var b = @new.Titles.Select(Key).ToHashSet();
        if (a.Count > 0 && b.Count > 0 && !a.Overlaps(b)) return new BroMessage(MessageCode.DiscChangedTitles, Severity.Info);
        return null;
    }

    /// <summary>Maps each of <paramref name="indices"/> (titles of <paramref name="old"/>) to a title of
    /// <paramref name="new"/>, preferring the same number. Throws <see cref="BroFailure"/> when a title is missing
    /// (disc.titleNotInListing, disc.titleGone), or when <paramref name="sameTracks"/> holds it and its track list
    /// changed (disc.tracksChanged).</summary>
    public static Dictionary<int, int> MapTitles(IEnumerable<int> indices, Listing old, Listing @new, IReadOnlySet<int> sameTracks)
    {
        var result = new Dictionary<int, int>();
        var used = new HashSet<int>();
        foreach (var i in indices.Distinct().OrderBy(i => i))
        {
            var t = old.Titles.FirstOrDefault(x => x.Index == i)
                ?? throw Fail(MessageCode.DiscTitleNotInListing, ("title", JsonValue.Of(i)));
            var candidates = @new.Titles.Where(x => Key(x) == Key(t) && !used.Contains(x.Index)).ToList();
            var match = candidates.FirstOrDefault(x => x.Index == i) ?? candidates.FirstOrDefault()
                ?? throw Fail(MessageCode.DiscTitleGone, ("title", JsonValue.Of(i)), ("duration", JsonValue.Of(t.Duration)),
                    ("source", JsonValue.Of(t.SourceFile.Length > 0 ? t.SourceFile : t.SourceTitleId is { } s ? "#" + s.ToString(CultureInfo.InvariantCulture) : "?")));
            if (sameTracks.Contains(i) && !t.Tracks.Select(x => x.Kind).SequenceEqual(match.Tracks.Select(x => x.Kind)))
                throw Fail(MessageCode.DiscTracksChanged, ("title", JsonValue.Of(i)));
            result[i] = match.Index;
            used.Add(match.Index);
        }
        return result;
    }

    internal static string Key(Title t) => FormattableString.Invariant($"{t.SourceTitleId ?? -1}|{t.DurationSeconds}|{t.SegmentMap}");

    private static BroFailure Fail(MessageCode code, params (string, JsonValue)[] p) =>
        new(new BroMessage(code, Severity.Error, p).ToError());
}
