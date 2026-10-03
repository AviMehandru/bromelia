using System.Collections.Generic;
using System.Linq;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>What <c>mkvmerge -J</c> says about a file: its duration (null when it reports none), its tracks and the
/// number of chapters.</summary>
public sealed record MkvProbe(double? DurationSeconds, IReadOnlyList<MkvTrack> Tracks, int ChapterCount)
{
    /// <summary><c>mkvmerge -J</c>'s JSON; null when it isn't JSON or mkvmerge didn't recognise the file.</summary>
    public static MkvProbe? Parse(string json)
    {
        var root = JsonValue.Parse(json);
        if (root?["container"] is not JsonValue.Object container) return null;
        if (container["recognized"]?.AsBool == false) return null;
        double? duration = container["properties"]?["duration"]?.AsNumber is { } ns ? ns / 1e9 : null;
        var tracks = (root["tracks"]?.AsArray ?? new List<JsonValue>())
            .Select(t => new MkvTrack((int)(t["id"]?.AsInteger ?? 0), t["type"]?.AsString ?? "")).ToList();
        int chapters = (root["chapters"]?.AsArray ?? new List<JsonValue>()).Sum(c => (int)(c["num_entries"]?.AsInteger ?? 0));
        return new MkvProbe(duration, tracks, chapters);
    }
}
