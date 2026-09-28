using System.Text;

namespace Bromelia.Core.Robot;

/// <summary>Attribute identifiers used in CINFO / TINFO / SINFO lines (MakeMKV apdefs.h).</summary>
public enum AttributeId
{
    Unknown = 0, Type = 1, Name = 2, LangCode = 3, LangName = 4, CodecId = 5, CodecShort = 6, CodecLong = 7,
    ChapterCount = 8, Duration = 9, DiskSize = 10, DiskSizeBytes = 11, StreamTypeExtension = 12, Bitrate = 13,
    AudioChannelsCount = 14, AngleInfo = 15, SourceFileName = 16, AudioSampleRate = 17, AudioSampleSize = 18,
    VideoSize = 19, VideoAspectRatio = 20, VideoFrameRate = 21, StreamFlags = 22, DateTime = 23,
    OriginalTitleId = 24, SegmentsCount = 25, SegmentsMap = 26, OutputFileName = 27, MetadataLanguageCode = 28,
    MetadataLanguageName = 29, TreeInfo = 30, PanelTitle = 31, VolumeName = 32, OrderWeight = 33,
    OutputFormat = 34, OutputFormatDescription = 35, SeamlessInfo = 36, PanelText = 37, MkvFlags = 38,
    MkvFlagsText = 39, AudioChannelLayoutName = 40, OutputCodecShort = 41, OutputConversionType = 42,
    OutputAudioSampleRate = 43, OutputAudioSampleSize = 44, OutputAudioChannelsCount = 45,
    OutputAudioChannelLayoutName = 46, OutputAudioChannelLayout = 47, OutputAudioMixDescription = 48,
    Comment = 49, OffsetSequenceId = 50,
}

public static class AttributeIdExtensions
{
    static readonly Dictionary<AttributeId, string> Names = new()
    {
        [AttributeId.Type] = "Type", [AttributeId.Name] = "Name", [AttributeId.LangCode] = "Language code",
        [AttributeId.LangName] = "Language", [AttributeId.CodecId] = "Codec ID", [AttributeId.CodecShort] = "Codec",
        [AttributeId.CodecLong] = "Codec (long)", [AttributeId.ChapterCount] = "Chapters", [AttributeId.Duration] = "Duration",
        [AttributeId.DiskSize] = "Size", [AttributeId.DiskSizeBytes] = "Size (bytes)", [AttributeId.Bitrate] = "Bitrate",
        [AttributeId.AudioChannelsCount] = "Channels", [AttributeId.AngleInfo] = "Angle", [AttributeId.SourceFileName] = "Source file",
        [AttributeId.AudioSampleRate] = "Sample rate", [AttributeId.AudioSampleSize] = "Sample size", [AttributeId.VideoSize] = "Resolution",
        [AttributeId.VideoAspectRatio] = "Aspect ratio", [AttributeId.VideoFrameRate] = "Frame rate", [AttributeId.StreamFlags] = "Stream flags",
        [AttributeId.DateTime] = "Date", [AttributeId.OriginalTitleId] = "Source title ID", [AttributeId.SegmentsCount] = "Segment count",
        [AttributeId.SegmentsMap] = "Segment map", [AttributeId.OutputFileName] = "Output file name",
        [AttributeId.MetadataLanguageCode] = "Metadata language code", [AttributeId.MetadataLanguageName] = "Metadata language",
        [AttributeId.VolumeName] = "Volume name", [AttributeId.OutputFormat] = "Output format",
        [AttributeId.OutputFormatDescription] = "Output format description", [AttributeId.SeamlessInfo] = "Seamless info",
        [AttributeId.MkvFlags] = "MKV flags", [AttributeId.MkvFlagsText] = "MKV flags (text)",
        [AttributeId.AudioChannelLayoutName] = "Channel layout", [AttributeId.OutputCodecShort] = "Output codec",
        [AttributeId.OutputConversionType] = "Conversion", [AttributeId.OutputAudioSampleRate] = "Output sample rate",
        [AttributeId.OutputAudioSampleSize] = "Output sample size", [AttributeId.OutputAudioChannelsCount] = "Output channels",
        [AttributeId.OutputAudioChannelLayoutName] = "Output channel layout", [AttributeId.OutputAudioMixDescription] = "Output mix",
        [AttributeId.Comment] = "Comment", [AttributeId.OffsetSequenceId] = "Offset sequence ID",
    };

    public static string DisplayName(this AttributeId id) => Names.TryGetValue(id, out var n) ? n : id.ToString();

    public static bool IsUserVisible(this AttributeId id) => id switch
    {
        AttributeId.Unknown or AttributeId.PanelTitle or AttributeId.PanelText or AttributeId.TreeInfo or
        AttributeId.OrderWeight or AttributeId.StreamTypeExtension or AttributeId.OutputAudioChannelLayout => false,
        _ => true,
    };
}

public enum DriveState
{
    EmptyClosed = 0,
    EmptyOpen = 1,
    Inserted = 2,
    Loading = 3,
    NoDrive = 256,
    Unmounting = 257,
}

public static class DriveStateExtensions
{
    public static DriveState FromRaw(int raw) => Enum.IsDefined(typeof(DriveState), raw) ? (DriveState)raw : DriveState.NoDrive;

    public static string DisplayName(this DriveState s) => s switch
    {
        DriveState.EmptyClosed => "No disc",
        DriveState.EmptyOpen => "Tray open",
        DriveState.Inserted => "Disc inserted",
        DriveState.Loading => "Loading…",
        DriveState.Unmounting => "Unmounting…",
        _ => "Not present",
    };
}

[Flags]
public enum DiscFlags
{
    None = 0,
    DvdFiles = 1,
    HdDvdFiles = 2,
    BlurayFiles = 4,
    AacsFiles = 8,
    BdsvmFiles = 16,
}

public static class DiscFlagsExtensions
{
    public static string DiscTypeName(this DiscFlags f)
    {
        if (f.HasFlag(DiscFlags.BlurayFiles)) return f.HasFlag(DiscFlags.AacsFiles) ? "Blu-ray (AACS)" : "Blu-ray";
        if (f.HasFlag(DiscFlags.HdDvdFiles)) return "HD DVD";
        if (f.HasFlag(DiscFlags.DvdFiles)) return "DVD";
        return "Disc";
    }
}

[Flags]
public enum StreamFlags
{
    None = 0,
    DirectorsComments = 1,
    AlternateDirectorsComments = 2,
    ForVisuallyImpaired = 4,
    CoreAudio = 256,
    SecondaryAudio = 512,
    HasCoreAudio = 1024,
    DerivedStream = 2048,
    ForcedSubtitles = 4096,
    ProfileSecondaryStream = 8192,
    OffsetSequenceIdPresent = 16384,
}

public static class StreamFlagsExtensions
{
    public static IReadOnlyList<string> Descriptions(this StreamFlags f)
    {
        var o = new List<string>();
        if (f.HasFlag(StreamFlags.DirectorsComments)) o.Add("Director's comments");
        if (f.HasFlag(StreamFlags.AlternateDirectorsComments)) o.Add("Alternate director's comments");
        if (f.HasFlag(StreamFlags.ForVisuallyImpaired)) o.Add("For visually impaired");
        if (f.HasFlag(StreamFlags.CoreAudio)) o.Add("Core audio");
        if (f.HasFlag(StreamFlags.SecondaryAudio)) o.Add("Secondary audio");
        if (f.HasFlag(StreamFlags.HasCoreAudio)) o.Add("Has core audio");
        if (f.HasFlag(StreamFlags.DerivedStream)) o.Add("Derived stream");
        if (f.HasFlag(StreamFlags.ForcedSubtitles)) o.Add("Forced subtitles");
        return o;
    }
}

public enum Severity { Debug, Info, Warning, Error }

public sealed record RobotMessage(int Code, int Flags, string Text, string Format, IReadOnlyList<string> Parameters)
{
    /// <summary>2018: write error (e.g. "No space left on device"); 5006: source file does not exist.</summary>
    static readonly HashSet<int> ErrorCodes = new() { 2003, 2004, 2018, 2023, 5003, 5006, 5010, 5021, 5037, 5055, 5069, 5077 };
    static readonly HashSet<int> WarningCodes = new() { 3038, 3041, 5042 };

    public Severity Severity
    {
        get
        {
            if (Code == 1003 || ((Flags & 0x20) != 0 && Text.StartsWith("DEBUG", StringComparison.Ordinal))) return Severity.Debug;
            if ((Flags & 0x200) != 0 || ErrorCodes.Contains(Code)) return Severity.Error;
            if ((Flags & 0x400) != 0 || WarningCodes.Contains(Code)) return Severity.Warning;
            return Severity.Info;
        }
    }
}

public sealed record DriveScanEntry(int Index, DriveState State, DiscFlags Flags, string DriveName, string DiscName, string DevicePath)
{
    public bool IsPresent => State != DriveState.NoDrive && !(DriveName.Length == 0 && DevicePath.Length == 0);
    public string LaneKey => DevicePath.Length == 0 ? $"disc:{Index}" : $"dev:{DevicePath}";
}

/// <summary>A parsed line of makemkvcon robot (-r) output.</summary>
public abstract record RobotEvent
{
    public sealed record Message(RobotMessage Value) : RobotEvent;
    /// <summary>PRGC — current (sub-)operation.</summary>
    public sealed record ProgressCurrentTitle(int Code, int Id, string Name) : RobotEvent;
    /// <summary>PRGT — total operation.</summary>
    public sealed record ProgressTotalTitle(int Code, int Id, string Name) : RobotEvent;
    /// <summary>PRGV — progress values; fractions are Current/Max and Total/Max.</summary>
    public sealed record ProgressValue(int Current, int Total, int Max) : RobotEvent;
    public sealed record Drive(DriveScanEntry Entry) : RobotEvent;
    public sealed record TitleCount(int Count) : RobotEvent;
    public sealed record DiscInfo(int Id, int Code, string Value) : RobotEvent;
    public sealed record TitleInfo(int Title, int Id, int Code, string Value) : RobotEvent;
    public sealed record StreamInfo(int Title, int Stream, int Id, int Code, string Value) : RobotEvent;
    public sealed record Raw(string Line) : RobotEvent;
}

public static class RobotParser
{
    /// <summary>Splits comma separated fields; quoted fields may contain commas and \" / \\ escapes.</summary>
    public static List<string> SplitFields(string s)
    {
        var fields = new List<string>();
        var cur = new StringBuilder();
        bool inQuotes = false, wasQuoted = false;
        for (int i = 0; i < s.Length; i++)
        {
            char c = s[i];
            if (inQuotes)
            {
                if (c == '\\' && i + 1 < s.Length)
                {
                    char n = s[i + 1];
                    if (n == '"' || n == '\\') { cur.Append(n); i++; }
                    else cur.Append(c);
                }
                else if (c == '"') inQuotes = false;
                else cur.Append(c);
            }
            else if (c == ',')
            {
                fields.Add(cur.ToString());
                cur.Clear();
                wasQuoted = false;
            }
            else if (c == '"' && cur.Length == 0 && !wasQuoted)
            {
                inQuotes = true;
                wasQuoted = true;
            }
            else cur.Append(c);
        }
        fields.Add(cur.ToString());
        return fields;
    }

    static bool Int(string s, out int v) => int.TryParse(s.Trim(), System.Globalization.NumberStyles.Integer,
        System.Globalization.CultureInfo.InvariantCulture, out v);

    public static RobotEvent? Parse(string rawLine)
    {
        var line = rawLine.TrimEnd('\r', '\n');
        if (line.Length == 0) return null;
        int colon = line.IndexOf(':');
        if (colon < 0) return new RobotEvent.Raw(line);
        var tag = line[..colon];
        var body = line[(colon + 1)..];
        List<string> f;
        switch (tag)
        {
            case "MSG":
                f = SplitFields(body);
                if (f.Count >= 5 && Int(f[0], out var code) && Int(f[1], out var flags))
                    return new RobotEvent.Message(new RobotMessage(code, flags, f[3], f[4], f.Skip(5).ToList()));
                break;
            case "PRGC":
            case "PRGT":
                f = SplitFields(body);
                if (f.Count >= 3 && Int(f[0], out var pc) && Int(f[1], out var pid))
                    return tag == "PRGC" ? new RobotEvent.ProgressCurrentTitle(pc, pid, f[2]) : new RobotEvent.ProgressTotalTitle(pc, pid, f[2]);
                break;
            case "PRGV":
                f = SplitFields(body);
                if (f.Count >= 3 && Int(f[0], out var a) && Int(f[1], out var b) && Int(f[2], out var m))
                    return new RobotEvent.ProgressValue(a, b, m);
                break;
            case "DRV":
                f = SplitFields(body);
                if (f.Count >= 7 && Int(f[0], out var idx) && Int(f[1], out var st) && Int(f[3], out var df))
                    return new RobotEvent.Drive(new DriveScanEntry(idx, DriveStateExtensions.FromRaw(st), (DiscFlags)df, f[4], f[5], f[6]));
                break;
            case "TCOUNT":
                if (Int(body, out var n)) return new RobotEvent.TitleCount(n);
                break;
            case "CINFO":
                f = SplitFields(body);
                if (f.Count >= 3 && Int(f[0], out var cid) && Int(f[1], out var ccode))
                    return new RobotEvent.DiscInfo(cid, ccode, f[2]);
                break;
            case "TINFO":
                f = SplitFields(body);
                if (f.Count >= 4 && Int(f[0], out var tt) && Int(f[1], out var tid) && Int(f[2], out var tcode))
                    return new RobotEvent.TitleInfo(tt, tid, tcode, f[3]);
                break;
            case "SINFO":
                f = SplitFields(body);
                if (f.Count >= 5 && Int(f[0], out var sti) && Int(f[1], out var ss) && Int(f[2], out var sid) && Int(f[3], out var scode))
                    return new RobotEvent.StreamInfo(sti, ss, sid, scode, f[4]);
                break;
        }
        return new RobotEvent.Raw(line);
    }
}

public enum NoticeKind
{
    /// <summary>"Using LibreDrive mode (v06.3 id=…)": the drive reads the disc in LibreDrive mode.</summary>
    LibreDrive,
    /// <summary>The disc (4K UHD) can only be decrypted by a LibreDrive-compatible drive, and this drive isn't one.</summary>
    LibreDriveRequired,
    /// <summary>The evaluation period / beta key has expired (messages 5052 and 5055).</summary>
    KeyExpired,
    /// <summary>The evaluation period hasn't been started; makemkvcon can't start it.</summary>
    EvaluationNotStarted,
    /// <summary>"This application version is too old": MakeMKV needs updating, or a purchased key.</summary>
    VersionTooOld,
}

/// <summary>Messages about the drive and about MakeMKV itself that Bromelia shows outside the log.</summary>
public sealed record MakeMKVNotice(NoticeKind Kind, string Detail = "")
{
    public static MakeMKVNotice? From(RobotMessage m)
    {
        var t = m.Text;
        if (t.StartsWith("Using LibreDrive mode", StringComparison.Ordinal))
        {
            int open = t.IndexOf('('), close = t.LastIndexOf(')');
            return new(NoticeKind.LibreDrive, open >= 0 && close > open ? t[(open + 1)..close] : "");
        }
        if (t.Contains("LibreDrive compatible drive is required", StringComparison.Ordinal)) return new(NoticeKind.LibreDriveRequired);
        if (m.Code is 5052 or 5055 || t.Contains("evaluation period has expired", StringComparison.OrdinalIgnoreCase)
            || t.Contains("evaluation period expired", StringComparison.OrdinalIgnoreCase)) return new(NoticeKind.KeyExpired);
        if (t.Contains("Evaluation period not started", StringComparison.Ordinal)
            || t.Contains("start MakeMKV evaluation from a third-party application", StringComparison.Ordinal)) return new(NoticeKind.EvaluationNotStarted);
        if (t.Contains("application version is too old", StringComparison.Ordinal)) return new(NoticeKind.VersionTooOld);
        return null;
    }

    /// <summary>MakeMKV can't (fully) work until the user acts: a key, an update, or starting the evaluation.</summary>
    public bool IsLicenseProblem => Kind is NoticeKind.KeyExpired or NoticeKind.EvaluationNotStarted or NoticeKind.VersionTooOld;

    /// <summary>What to tell the user, for banners and failed jobs.</summary>
    public string Explanation => Kind switch
    {
        NoticeKind.LibreDrive => $"The drive reads this disc in LibreDrive mode{(Detail.Length > 0 ? $" ({Detail})" : "")}.",
        NoticeKind.LibreDriveRequired => "This disc can only be decrypted by a LibreDrive-compatible drive, and this drive isn't one (or its firmware isn't supported).",
        NoticeKind.KeyExpired => "MakeMKV's key has expired. Blu-ray and 4K UHD discs can't be opened until you enter the current beta key or a purchased key.",
        NoticeKind.EvaluationNotStarted => "MakeMKV's evaluation hasn't been started. Open the MakeMKV app once, or enter a beta or purchased key.",
        _ => "This MakeMKV version is too old. Update MakeMKV, or enter a purchased key to keep using this version.",
    };
}
