using System.Globalization;

namespace Bromelia.Core.Robot;

public enum TrackKind { Video, Audio, Subtitle, Attachment, Other }

public sealed class TrackInfo
{
    public int Index { get; init; }
    public Dictionary<int, string> Attributes { get; init; } = new();

    public string? Attr(AttributeId a) => Attributes.TryGetValue((int)a, out var v) ? v : null;

    public TrackKind Kind => Attr(AttributeId.Type)?.ToLowerInvariant() switch
    {
        "video" => TrackKind.Video,
        "audio" => TrackKind.Audio,
        "subtitles" or "subtitle" => TrackKind.Subtitle,
        "attachment" => TrackKind.Attachment,
        _ => TrackKind.Other,
    };

    public string LanguageCode => Attr(AttributeId.LangCode) ?? "";
    public string LanguageName => Attr(AttributeId.LangName) ?? "";
    public string Codec => Attr(AttributeId.CodecShort) ?? Attr(AttributeId.CodecId) ?? "";
    public StreamFlags Flags => (StreamFlags)(int.TryParse(Attr(AttributeId.StreamFlags), out var f) ? f : 0);
    public bool IsDefault => (Attr(AttributeId.MkvFlags) ?? "").Contains('d');

    public string Summary
    {
        get
        {
            var tree = Attr(AttributeId.TreeInfo)?.Trim();
            if (!string.IsNullOrEmpty(tree)) return tree;
            return string.Join(" ", new[] { Attr(AttributeId.CodecShort), Attr(AttributeId.Name), Attr(AttributeId.LangName) }
                .Where(s => !string.IsNullOrEmpty(s)));
        }
    }
}

public sealed class TitleInfo
{
    public int Index { get; init; }
    public Dictionary<int, string> Attributes { get; init; } = new();
    public List<TrackInfo> Tracks { get; init; } = new();

    public string? Attr(AttributeId a) => Attributes.TryGetValue((int)a, out var v) ? v : null;

    public string Name => Attr(AttributeId.Name) ?? "";
    public int ChapterCount => int.TryParse(Attr(AttributeId.ChapterCount), out var n) ? n : 0;
    public string DurationText => Attr(AttributeId.Duration) ?? "";
    public int DurationSeconds => ParseDuration(DurationText);
    public long SizeBytes => long.TryParse(Attr(AttributeId.DiskSizeBytes), out var n) ? n : 0;
    public string SizeText => Attr(AttributeId.DiskSize) ?? FormatBytes(SizeBytes);
    public int? SourceTitleId => int.TryParse(Attr(AttributeId.OriginalTitleId), out var n) ? n : null;
    public string SegmentMap => Attr(AttributeId.SegmentsMap) ?? "";
    public string OutputFileName => Attr(AttributeId.OutputFileName) ?? "";
    public string SourceFileName => Attr(AttributeId.SourceFileName) ?? "";
    public string Comment => Attr(AttributeId.Comment) ?? "";
    public int? Angle => int.TryParse(Attr(AttributeId.AngleInfo), out var n) ? n : null;

    public static int ParseDuration(string s)
    {
        int total = 0;
        foreach (var part in s.Split(':'))
            total = total * 60 + (int.TryParse(part.Trim(), out var n) ? n : 0);
        return total;
    }

    public static string FormatDuration(int seconds) =>
        string.Format(CultureInfo.InvariantCulture, "{0}:{1:00}:{2:00}", seconds / 3600, seconds / 60 % 60, seconds % 60);

    public static string FormatBytes(long bytes)
    {
        string[] units = { "B", "KB", "MB", "GB", "TB" };
        double v = bytes;
        int u = 0;
        while (v >= 1000 && u < units.Length - 1) { v /= 1000; u++; }
        return u == 0 ? $"{bytes} B" : v.ToString("0.0", CultureInfo.InvariantCulture) + " " + units[u];
    }
}

public sealed class DiscInfo
{
    public Dictionary<int, string> Attributes { get; init; } = new();
    public List<TitleInfo> Titles { get; init; } = new();
    public int ReportedTitleCount { get; set; }

    public string? Attr(AttributeId a) => Attributes.TryGetValue((int)a, out var v) ? v : null;
    public string Name => Attr(AttributeId.Name) ?? Attr(AttributeId.VolumeName) ?? "";
    public string VolumeName => Attr(AttributeId.VolumeName) ?? "";
    public string TypeName => Attr(AttributeId.Type) ?? "";

    public string TypeToken
    {
        get
        {
            var t = TypeName.ToLowerInvariant();
            if (t.Contains("blu")) return "bd";
            if (t.Contains("hd")) return "hddvd";
            if (t.Contains("dvd")) return "dvd";
            return "disc";
        }
    }

    public TitleInfo? Title(int index) => Titles.FirstOrDefault(t => t.Index == index);
}

public sealed class DiscInfoBuilder
{
    public DiscInfo Info { get; } = new();

    public void Consume(RobotEvent ev)
    {
        switch (ev)
        {
            case RobotEvent.TitleCount tc:
                Info.ReportedTitleCount = tc.Count;
                break;
            case RobotEvent.DiscInfo ci:
                Info.Attributes[ci.Id] = ci.Value;
                break;
            case RobotEvent.TitleInfo ti:
                EnsureTitle(ti.Title).Attributes[ti.Id] = ti.Value;
                break;
            case RobotEvent.StreamInfo si:
                var title = EnsureTitle(si.Title);
                var track = title.Tracks.FirstOrDefault(t => t.Index == si.Stream);
                if (track == null)
                {
                    track = new TrackInfo { Index = si.Stream };
                    title.Tracks.Add(track);
                    title.Tracks.Sort((a, b) => a.Index.CompareTo(b.Index));
                }
                track.Attributes[si.Id] = si.Value;
                break;
        }
    }

    TitleInfo EnsureTitle(int index)
    {
        var t = Info.Titles.FirstOrDefault(x => x.Index == index);
        if (t != null) return t;
        t = new TitleInfo { Index = index };
        Info.Titles.Add(t);
        Info.Titles.Sort((a, b) => a.Index.CompareTo(b.Index));
        return t;
    }

    public static DiscInfo Build(string output)
    {
        var b = new DiscInfoBuilder();
        foreach (var line in output.Split('\n'))
            if (RobotParser.Parse(line) is { } ev) b.Consume(ev);
        return b.Info;
    }
}
