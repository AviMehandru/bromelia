using System.Collections.Generic;
using System.Linq;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>Builds a <see cref="Listing"/> from robot events (TCOUNT, CINFO, TINFO, SINFO; others are ignored).</summary>
public sealed class ListingBuilder
{
    private int _reportedTitleCount;
    private readonly Dictionary<int, string> _disc = new();
    private readonly SortedDictionary<int, Dictionary<int, string>> _titles = new();
    private readonly SortedDictionary<int, SortedDictionary<int, Dictionary<int, string>>> _tracks = new();

    public void Feed(RobotEvent @event)
    {
        switch (@event)
        {
            case RobotEvent.TitleCount c:
                _reportedTitleCount = c.Count;
                break;
            case RobotEvent.DiscInfo d:
                _disc[d.Id] = d.Value;
                break;
            case RobotEvent.TitleInfo t:
                TitleAttributes(t.Title)[t.Id] = t.Value;
                break;
            case RobotEvent.StreamInfo s:
                TitleAttributes(s.Title);
                var tracks = _tracks[s.Title];
                if (!tracks.TryGetValue(s.Stream, out var attrs)) tracks[s.Stream] = attrs = new Dictionary<int, string>();
                attrs[s.Id] = s.Value;
                break;
        }
    }

    public Listing Build()
    {
        var titles = _titles.Select(t => BuildTitle(t.Key, t.Value, _tracks[t.Key])).ToList();
        var typeText = Get(_disc, AttributeId.Type) ?? "";
        return new Listing(
            Get(_disc, AttributeId.Name) ?? Get(_disc, AttributeId.VolumeName) ?? "",
            Get(_disc, AttributeId.VolumeName) ?? "",
            TypeOf(typeText),
            typeText,
            _reportedTitleCount,
            titles,
            new Dictionary<int, string>(_disc));
    }

    private Dictionary<int, string> TitleAttributes(int title)
    {
        if (!_titles.TryGetValue(title, out var attrs))
        {
            _titles[title] = attrs = new Dictionary<int, string>();
            _tracks[title] = new SortedDictionary<int, Dictionary<int, string>>();
        }
        return attrs;
    }

    private static Title BuildTitle(int index, Dictionary<int, string> a, SortedDictionary<int, Dictionary<int, string>> tracks)
    {
        var duration = Get(a, AttributeId.Duration) ?? "";
        return new Title(
            index,
            IntOf(Get(a, AttributeId.OriginalTitleId)),
            Get(a, AttributeId.SourceFileName) ?? "",
            Get(a, AttributeId.Name) ?? "",
            Get(a, AttributeId.Comment) ?? "",
            duration,
            (int)(Foundation.Duration.ParseClock(duration)?.Seconds ?? 0),
            IntOf(Get(a, AttributeId.ChapterCount)) ?? 0,
            LongOf(Get(a, AttributeId.DiskSizeBytes)) ?? 0,
            Get(a, AttributeId.SegmentsMap) ?? "",
            Get(a, AttributeId.OutputFileName) ?? "",
            IntOf(Get(a, AttributeId.AngleInfo)),
            tracks.Select(t => BuildTrack(t.Key, t.Value)).ToList(),
            new Dictionary<int, string>(a));
    }

    private static Track BuildTrack(int index, Dictionary<int, string> a) => new(
        index,
        KindOf(Get(a, AttributeId.Type)),
        Get(a, AttributeId.CodecShort) ?? Get(a, AttributeId.CodecId) ?? "",
        Get(a, AttributeId.LangCode) ?? "",
        Get(a, AttributeId.LangName) ?? "",
        Get(a, AttributeId.Name) ?? "",
        (Get(a, AttributeId.MkvFlags) ?? "").Contains('d'),
        new Dictionary<int, string>(a));

    private static string? Get(Dictionary<int, string> a, AttributeId id) => a.TryGetValue((int)id, out var v) ? v : null;

    private static int? IntOf(string? s) => s != null && Robot.Int(s, out var v) ? v : null;

    private static long? LongOf(string? s) =>
        s != null && long.TryParse(s.Trim(' ', '\t'), System.Globalization.NumberStyles.AllowLeadingSign, System.Globalization.CultureInfo.InvariantCulture, out var v) ? v : null;

    private static TrackKind KindOf(string? type) => type is null ? TrackKind.Unknown : MessageCatalog.AsciiLower(type) switch
    {
        "video" => TrackKind.Video,
        "audio" => TrackKind.Audio,
        "subtitles" or "subtitle" => TrackKind.Subtitle,
        "attachment" => TrackKind.Attachment,
        _ => TrackKind.Unknown,
    };

    private static DiscType TypeOf(string typeText)
    {
        var t = MessageCatalog.AsciiLower(typeText);
        if (t.Contains("blu")) return DiscType.Bd;
        if (t.Contains("hd")) return DiscType.Hddvd;
        if (t.Contains("dvd")) return DiscType.Dvd;
        return DiscType.Disc;
    }
}
