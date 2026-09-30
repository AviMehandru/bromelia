using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;
using Bromelia.Core.Config;
using Bromelia.Core.Robot;

namespace Bromelia.Core.Logic;

// What a disc *is*: its format (DVD / Blu-ray / 4K UHD), the movie or show on it, and where it sits in a
// set (season, part, volume, disc number). Used for file names, plugin matching and the archive record.
// Same rules as the macOS and Linux versions.

public enum DiscFormat { Dvd, Bluray, Uhd, Hddvd, Unknown }

public enum MediaKind { Movie, Tv }

public static class DiscFormatExtensions
{
    /// <summary>Every format code, for pickers and validation.</summary>
    public static readonly string[] AllCodes = { "DVD", "DVDe", "BR", "BRe", "4K", "4Ke", "HDDVD", "HDDVDe" };

    /// <summary>Format code used in file names. Encrypted (not decrypted) backups get an "e" suffix.</summary>
    public static string Code(this DiscFormat f, bool encrypted)
    {
        var b = f switch
        {
            DiscFormat.Dvd => "DVD",
            DiscFormat.Bluray => "BR",
            DiscFormat.Uhd => "4K",
            DiscFormat.Hddvd => "HDDVD",
            _ => "DISC",
        };
        return encrypted ? b + "e" : b;
    }

    public static string Label(this DiscFormat f) => f switch
    {
        DiscFormat.Dvd => "DVD",
        DiscFormat.Bluray => "Blu-ray",
        DiscFormat.Uhd => "4K Ultra HD Blu-ray",
        DiscFormat.Hddvd => "HD DVD",
        _ => "Disc",
    };

    public static string Token(this DiscFormat f) => f switch
    {
        DiscFormat.Dvd => "dvd",
        DiscFormat.Bluray => "bluray",
        DiscFormat.Uhd => "uhd",
        DiscFormat.Hddvd => "hddvd",
        _ => "unknown",
    };

    public static string Label(this MediaKind k) => k == MediaKind.Movie ? "Movie" : "TV show";
    public static string Token(this MediaKind k) => k == MediaKind.Movie ? "movie" : "tv";

    /// <summary>Detects the format from makemkvcon's listing, falling back to the drive's flags.
    /// UHD discs are Blu-rays whose video is 2160p or HEVC.</summary>
    public static DiscFormat Detect(DiscInfo? info, DiscFlags? flags = null)
    {
        if (info != null)
        {
            var t = info.TypeName.ToLowerInvariant();
            if (t.Contains("blu")) return IsUhd(info) ? DiscFormat.Uhd : DiscFormat.Bluray;
            if (t.Contains("hd")) return DiscFormat.Hddvd;
            if (t.Contains("dvd")) return DiscFormat.Dvd;
            if (IsUhd(info)) return DiscFormat.Uhd;
        }
        if (flags is { } f)
        {
            if (f.HasFlag(DiscFlags.BlurayFiles)) return DiscFormat.Bluray;
            if (f.HasFlag(DiscFlags.HdDvdFiles)) return DiscFormat.Hddvd;
            if (f.HasFlag(DiscFlags.DvdFiles)) return DiscFormat.Dvd;
        }
        return DiscFormat.Unknown;
    }

    public static bool IsUhd(DiscInfo info) =>
        info.Titles.Any(t => t.Tracks.Any(tr =>
        {
            if (tr.Kind != TrackKind.Video) return false;
            var size = tr.Attr(AttributeId.VideoSize) ?? "";
            var codec = ((tr.Attr(AttributeId.CodecId) ?? "") + " " + (tr.Attr(AttributeId.CodecShort) ?? "")).ToUpperInvariant();
            return size.Contains("2160") || size.Contains("3840") || codec.Contains("HEVC") || codec.Contains("MPEGH");
        }));

    /// <summary>Backup folders: VIDEO_TS = DVD; BDMV/index.bdmv starts with "INDX0300" on UHD discs.</summary>
    public static DiscFormat? DetectBackupFolder(string folder)
    {
        if (Directory.Exists(Path.Combine(folder, "VIDEO_TS"))) return DiscFormat.Dvd;
        var index = Path.Combine(folder, "BDMV", "index.bdmv");
        if (!File.Exists(index)) return null;
        try
        {
            using var fs = File.OpenRead(index);
            var head = new byte[8];
            int n = fs.Read(head, 0, 8);
            var s = Encoding.ASCII.GetString(head, 0, n);
            if (!s.StartsWith("INDX", StringComparison.Ordinal)) return null;
            return s == "INDX0300" ? DiscFormat.Uhd : DiscFormat.Bluray;
        }
        catch (IOException) { return null; }
    }
}

/// <summary>Result of reading a label such as ONE_PIECE_S2_P7_D2.</summary>
public sealed record LabelInfo
{
    public string Title { get; set; } = "";
    public int? Season { get; set; }
    public int? Part { get; set; }
    public int? Volume { get; set; }
    public int? Disc { get; set; }
    public bool LooksLikeSeries { get; set; }

    /// <summary>"Season 2 Part 7 Disc 2", or "" when the label has no set information.</summary>
    public string SetDescription
    {
        get
        {
            var parts = new List<string>();
            if (Season is { } s) parts.Add($"Season {s}");
            if (Part is { } p) parts.Add($"Part {p}");
            if (Volume is { } v) parts.Add($"Volume {v}");
            if (Disc is { } d) parts.Add($"Disc {d}");
            return string.Join(" ", parts);
        }
    }
}

public static class LabelParser
{
    static readonly HashSet<string> Noise = new(StringComparer.Ordinal)
    {
        "WS", "FS", "16X9", "4X3", "NTSC", "PAL", "R1", "R2", "R4", "UHD", "4K", "BD", "BLURAY", "BLU", "RAY",
        "DVD", "DVD5", "DVD9", "BD25", "BD50", "BD66", "BD100", "HDR", "SDR", "HD", "DISC", "DISK",
    };
    static readonly HashSet<string> SmallWords = new(StringComparer.Ordinal)
    {
        "a", "an", "and", "as", "at", "but", "by", "for", "from", "in", "into", "nor", "of", "on", "or", "the", "to", "vs", "with",
    };

    static readonly Regex SeasonDisc = new(@"^S(\d{1,2})(?:D(\d{1,2}))?(?:E(\d{1,3}))?$", RegexOptions.CultureInvariant);
    static readonly Regex Season = new(@"^SEASON(\d{1,2})?$", RegexOptions.CultureInvariant);
    static readonly Regex Episode = new(@"^(?:EP|EPS|EPISODE|EPISODES)(\d{1,3})?$", RegexOptions.CultureInvariant);
    static readonly Regex Disc = new(@"^(?:D|DISC|DISK|CD)(\d{1,2})$", RegexOptions.CultureInvariant);
    static readonly Regex Part = new(@"^(?:P|PT|PART)(\d{1,2})$", RegexOptions.CultureInvariant);
    static readonly Regex Volume = new(@"^(?:V|VOL|VOLUME)(\d{1,2})$", RegexOptions.CultureInvariant);

    /// <summary>Splits a label into words, reads set markers (S2, SEASON 2, P7, VOL 3, D2, DISC 2, S1D2 …) and
    /// returns the cleaned title. Everything from the first set marker on is left out of the title.</summary>
    public static LabelInfo Parse(string raw)
    {
        var info = new LabelInfo();
        var s = raw.Replace("™", "").Replace("®", "").Replace("©", "").Replace(" - ", " ");
        var tokens = s.Split(new[] { '_', '.', ' ', '\t', '(', ')', '[', ']', ',' }, StringSplitOptions.RemoveEmptyEntries)
            .Where(t => t != "-").ToList();
        var titleTokens = new List<string>();
        bool stopped = false;
        int? NumberAfter(int i) => i + 1 < tokens.Count && int.TryParse(tokens[i + 1], NumberStyles.None, CultureInfo.InvariantCulture, out var n) ? n : null;

        for (int i = 0; i < tokens.Count; i++)
        {
            var t = tokens[i];
            var u = t.ToUpperInvariant();
            bool marker = true, consumed = false;
            Match m;
            if ((m = SeasonDisc.Match(u)).Success)
            {
                info.Season = int.Parse(m.Groups[1].Value, CultureInfo.InvariantCulture);
                if (m.Groups[2].Success) info.Disc = int.Parse(m.Groups[2].Value, CultureInfo.InvariantCulture);
                info.LooksLikeSeries = true;
            }
            else if ((m = Season.Match(u)).Success)
            {
                if (m.Groups[1].Success) info.Season = int.Parse(m.Groups[1].Value, CultureInfo.InvariantCulture);
                else if (NumberAfter(i) is { } n) { info.Season = n; consumed = true; }
                info.LooksLikeSeries = true;
            }
            else if ((m = Episode.Match(u)).Success)
            {
                if (!m.Groups[1].Success && NumberAfter(i) != null) consumed = true;
                info.LooksLikeSeries = true;
            }
            else if ((m = Disc.Match(u)).Success) info.Disc = int.Parse(m.Groups[1].Value, CultureInfo.InvariantCulture);
            else if (u is "DISC" or "DISK" or "D" && NumberAfter(i) is { } dn) { info.Disc = dn; consumed = true; }
            else if ((m = Part.Match(u)).Success) info.Part = int.Parse(m.Groups[1].Value, CultureInfo.InvariantCulture);
            else if (u is "PART" or "PT" && NumberAfter(i) is { } pn) { info.Part = pn; consumed = true; }
            else if ((m = Volume.Match(u)).Success) { info.Volume = int.Parse(m.Groups[1].Value, CultureInfo.InvariantCulture); info.LooksLikeSeries = true; }
            else if (u is "VOL" or "VOLUME" && NumberAfter(i) is { } vn) { info.Volume = vn; consumed = true; info.LooksLikeSeries = true; }
            else marker = false;

            if (marker) stopped = true;
            else if (!stopped && !Noise.Contains(u)) titleTokens.Add(t);
            if (consumed) i++;
        }
        info.Title = TitleCase(titleTokens);
        return info;
    }

    /// <summary>Title-cases words when the label is all upper or all lower case; mixed case is kept.</summary>
    public static string TitleCase(IReadOnlyList<string> words)
    {
        bool shouting = words.All(w => w == w.ToUpperInvariant()) || words.All(w => w == w.ToLowerInvariant());
        if (!shouting) return string.Join(" ", words);
        var outWords = new List<string>();
        for (int i = 0; i < words.Count; i++)
        {
            var w = words[i];
            var lower = w.ToLowerInvariant();
            if (i > 0 && SmallWords.Contains(lower)) { outWords.Add(lower); continue; }
            if (w.Length <= 4 && lower.All(c => c is 'i' or 'v' or 'x')) { outWords.Add(w.ToUpperInvariant()); continue; }
            outWords.Add(string.Join("-", lower.Split('-').Select(p => p.Length == 0 ? p : char.ToUpperInvariant(p[0]) + p[1..])));
        }
        return string.Join(" ", outWords);
    }
}

/// <summary>The movie or show on a disc, as used for naming.</summary>
public sealed record MediaIdentity(string Name, MediaKind Kind, DiscFormat Format, bool Encrypted, LabelInfo Label, string Reason)
{
    public string FormatCode => Format.Code(Encrypted);

    /// <summary>Titles of 10–75 minutes within ±35 % of the median of such titles.</summary>
    public static List<TitleInfo> EpisodeLikeTitles(IEnumerable<TitleInfo> titles)
    {
        var candidates = titles.Where(t => t.DurationSeconds is >= 600 and <= 4500).ToList();
        if (candidates.Count < 2) return new List<TitleInfo>();
        var sorted = candidates.Select(t => t.DurationSeconds).OrderBy(d => d).ToList();
        double median = sorted[sorted.Count / 2];
        return candidates.Where(t => Math.Abs(t.DurationSeconds - median) <= median * 0.35).ToList();
    }

    public static MediaIdentity Resolve(DiscInfo? info, string discLabel, bool encrypted, DiscFlags? flags = null, DiscFormat? format = null,
        string nameOverride = "", MediaKind? kindOverride = null, int playAllEpisodes = 0)
    {
        var volume = info is { VolumeName.Length: > 0 } ? info.VolumeName : discLabel;
        var discName = info?.Name ?? "";
        var fromVolume = LabelParser.Parse(volume);
        bool nameLooksHuman = discName.Length > 0 && !discName.Contains('_') && discName != volume
                              && (discName.Contains(' ') || discName != discName.ToUpperInvariant());
        var fromName = LabelParser.Parse(discName);
        var label = fromVolume with
        {
            Title = fromVolume.Title.Length > 0 ? fromVolume.Title : fromName.Title,
            Season = fromVolume.Season ?? fromName.Season,
            Disc = fromVolume.Disc ?? fromName.Disc,
            Part = fromVolume.Part ?? fromName.Part,
            Volume = fromVolume.Volume ?? fromName.Volume,
            LooksLikeSeries = fromVolume.LooksLikeSeries || fromName.LooksLikeSeries,
        };

        var name = nameOverride.Trim();
        if (name.Length == 0) name = nameLooksHuman && fromName.Title.Length > 0 ? fromName.Title : label.Title;
        if (name.Length == 0) name = discLabel.Length == 0 ? "Disc" : discLabel;

        var fmt = format ?? DiscFormatExtensions.Detect(info, flags);
        MediaKind kind;
        string reason;
        if (kindOverride is { } k) { kind = k; reason = "chosen by you"; }
        else if (label.LooksLikeSeries) { kind = MediaKind.Tv; reason = "the disc label has season / volume markers"; }
        else if (playAllEpisodes >= 3) { kind = MediaKind.Tv; reason = $"the disc menu plays {playAllEpisodes} episodes in one title"; }
        else if (EpisodeLikeTitles(info?.Titles ?? new List<TitleInfo>()).Count >= 3) { kind = MediaKind.Tv; reason = "the disc has several titles of episode length"; }
        else { kind = MediaKind.Movie; reason = "no series markers or episode-length titles"; }
        return new MediaIdentity(name, kind, fmt, encrypted, label, reason);
    }

    static string N(int? v) => v?.ToString(CultureInfo.InvariantCulture) ?? "";

    /// <summary>Template values shared by every file of the job.</summary>
    public Dictionary<string, string> TemplateValues(string rip) => new()
    {
        ["name"] = Name,
        ["kind"] = Kind.Token(),
        ["format"] = FormatCode,
        ["rip"] = rip,
        ["discLabel"] = Label.SetDescription,
        ["discNumber"] = N(Label.Disc),
        ["season"] = N(Label.Season),
        ["part"] = N(Label.Part),
        ["volumeNumber"] = N(Label.Volume),
        ["episode"] = "",
        ["episodeNumber"] = "",
        ["episodeTitle"] = "",
        ["track"] = "",
    };

    /// <summary>"Title 11" for DVD titles, "Playlist 00800" for Blu-ray playlists, else MakeMKV's title number.</summary>
    public static string TrackLabel(TitleInfo t)
    {
        var src = t.SourceFileName;
        if (src.EndsWith(".mpls", StringComparison.OrdinalIgnoreCase)) return "Playlist " + Path.GetFileNameWithoutExtension(src);
        return $"Title {(t.SourceTitleId ?? t.Index).ToString(CultureInfo.InvariantCulture)}";
    }

    public static string EpisodeLabel(int n, int width) => "Episode " + n.ToString(CultureInfo.InvariantCulture).PadLeft(width, '0');
}

/// <summary>Decides which post-processing steps apply to a disc: MatchName is a case-insensitive regular
/// expression tested against the name and the disc label; MatchFormats lists format codes (a trailing *
/// matches any code with that prefix). Empty conditions match everything.</summary>
public static class PluginMatcher
{
    public static bool Matches(PostProcessStep step, string name, string discLabel, string formatCode)
    {
        var pattern = step.MatchName.Trim();
        if (pattern.Length > 0)
        {
            Regex re;
            try { re = new Regex(pattern, RegexOptions.IgnoreCase | RegexOptions.CultureInvariant); }
            catch (ArgumentException) { return false; }
            if (!re.IsMatch(name) && !re.IsMatch(discLabel)) return false;
        }
        var formats = step.MatchFormats.Select(f => f.Trim()).Where(f => f.Length > 0).ToList();
        if (formats.Count == 0) return true;
        return formats.Any(f => f.EndsWith('*')
            ? formatCode.StartsWith(f[..^1], StringComparison.OrdinalIgnoreCase)
            : string.Equals(f, formatCode, StringComparison.OrdinalIgnoreCase));
    }

    public static string? Validate(string pattern)
    {
        if (string.IsNullOrWhiteSpace(pattern)) return null;
        try { _ = new Regex(pattern.Trim()); return null; }
        catch (ArgumentException) { return "Invalid regular expression"; }
    }
}
