using System.Text.Json;
using System.Text.Json.Serialization;
using Bromelia.Core.Robot;

namespace Bromelia.Core.Config;

// Same JSON schema as the macOS and Linux versions (docs/configuration.md). Missing properties
// keep their defaults and unknown properties are ignored, so files move freely between versions.

public enum TitleStrategy { All, Longest, Indices, Manual }
public enum IndexBase { Makemkv, Source }
public enum RipMode { Mkv, Backup, BackupDecrypted, BackupThenMkv, InfoOnly }
public enum BackupFormat { Folder, Iso }
public enum ConflictPolicy { UniqueSuffix, Overwrite, Skip }
public enum RunCondition { Success, Failure, Always }
public enum ProfileMode { MakemkvDefault, Generated, CustomFile }

// Serialized as "copy", "lpcm", "wavex", "flac-best", "flac-fast" (see LpcmOutputConverter on the properties).
public enum LpcmOutput { Copy, Lpcm, Wavex, FlacBest, FlacFast }

public static class EnumLabels
{
    public static string Label(this RipMode m) => m switch
    {
        RipMode.Mkv => "Rip titles to MKV",
        RipMode.Backup => "Backup (encrypted, 1:1)",
        RipMode.BackupDecrypted => "Backup (decrypted)",
        RipMode.BackupThenMkv => "Decrypted backup, then MKV from backup",
        _ => "Scan only (save disc information)",
    };

    public static string ShortLabel(this RipMode m) => m switch
    {
        RipMode.Mkv => "MKV",
        RipMode.Backup => "Backup",
        RipMode.BackupDecrypted => "Decrypted backup",
        RipMode.BackupThenMkv => "Backup + MKV",
        _ => "Scan",
    };

    public static bool MakesMkv(this RipMode m) => m is RipMode.Mkv or RipMode.BackupThenMkv;
    public static bool MakesBackup(this RipMode m) => m is RipMode.Backup or RipMode.BackupDecrypted or RipMode.BackupThenMkv;

    public static string Label(this TitleStrategy s) => s switch
    {
        TitleStrategy.All => "All titles",
        TitleStrategy.Longest => "Longest title(s)",
        TitleStrategy.Indices => "Titles matching an index pattern",
        _ => "Choose manually",
    };

    public static string Label(this RunCondition r) => r switch
    {
        RunCondition.Success => "When the rip succeeds",
        RunCondition.Failure => "When the rip fails",
        _ => "Always",
    };

    public static string Label(this LpcmOutput o) => o switch
    {
        LpcmOutput.Copy => "Copy as is",
        LpcmOutput.Lpcm => "Raw LPCM",
        LpcmOutput.Wavex => "LPCM in WAV container",
        LpcmOutput.FlacBest => "FLAC (best compression)",
        _ => "FLAC (fast compression)",
    };

    public static string ProfileName(this LpcmOutput o) => o switch
    {
        LpcmOutput.Copy => "copy",
        LpcmOutput.Lpcm => "lpcm",
        LpcmOutput.Wavex => "wavex",
        LpcmOutput.FlacBest => "flac-best",
        _ => "flac-fast",
    };

    public static string Label(this ProfileMode m) => m switch
    {
        ProfileMode.MakemkvDefault => "MakeMKV default profile",
        ProfileMode.Generated => "Bromelia profile (edit below)",
        _ => "Custom profile file (.mmcp.xml)",
    };

    public static string Label(this ConflictPolicy p) => p switch
    {
        ConflictPolicy.UniqueSuffix => "Add a number (Disc (2))",
        ConflictPolicy.Overwrite => "Reuse the existing folder",
        _ => "Skip the job",
    };
}

sealed class LpcmOutputConverter : JsonConverter<LpcmOutput>
{
    public override LpcmOutput Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options) =>
        reader.GetString() switch
        {
            "copy" => LpcmOutput.Copy,
            "lpcm" => LpcmOutput.Lpcm,
            "wavex" => LpcmOutput.Wavex,
            "flac-fast" => LpcmOutput.FlacFast,
            _ => LpcmOutput.FlacBest,
        };

    public override void Write(Utf8JsonWriter writer, LpcmOutput value, JsonSerializerOptions options) =>
        writer.WriteStringValue(value.ProfileName());
}

public sealed class TitleSelection
{
    public TitleStrategy Strategy { get; set; } = TitleStrategy.All;
    public int LongestCount { get; set; } = 1;
    public string IndexPattern { get; set; } = "";
    public IndexBase IndexBase { get; set; } = IndexBase.Makemkv;
    public int MinDurationSeconds { get; set; }
    public int MaxDurationSeconds { get; set; }
    public int MinChapters { get; set; }
    public int MaxChapters { get; set; }
    public int MinSizeMB { get; set; }
    public int MaxSizeMB { get; set; }
    public string IncludePattern { get; set; } = "";
    public string ExcludePattern { get; set; } = "";
    public bool SkipDuplicates { get; set; } = true;
    public bool SkipAlternateAngles { get; set; }
    public int MaxTitles { get; set; }
}

public sealed class RipConfig
{
    public RipMode Mode { get; set; } = RipMode.Mkv;
    public BackupFormat BackupFormat { get; set; } = BackupFormat.Folder;
    [JsonPropertyName("keepBackupAfterMKV")]
    public bool KeepBackupAfterMkv { get; set; } = true;
    public TitleSelection TitleSelection { get; set; } = new();
    public int? MinLengthSeconds { get; set; }
    public int? CacheMB { get; set; }
    public bool? DirectIO { get; set; }
    public string ExtraArguments { get; set; } = "";
    [JsonPropertyName("writeDiscInfoJSON")]
    public bool WriteDiscInfoJson { get; set; }
}

public sealed class OutputConfig
{
    /// <summary>{name} - {episode} - {discLabel} - {rip} - {track} - {format}, leaving out parts that don't apply.</summary>
    public const string DefaultFileNameTemplate = "{name}{episode? - {episode}}{discLabel? - {discLabel}} - {rip}{track? - {track}} - {format}";
    public const string DefaultFolderTemplate = "{name}{discLabel? - {discLabel}}";
    /// <summary>Folder template of configuration version 1, replaced when a configuration is upgraded.</summary>
    public const string LegacyFolderTemplate = "{disc}";

    public string RootOverride { get; set; } = "";
    public string FolderTemplate { get; set; } = DefaultFolderTemplate;
    /// <summary>Used for MKV files and backups (without extension). Empty = keep MakeMKV's names.</summary>
    public string FileNameTemplate { get; set; } = DefaultFileNameTemplate;
    public string BackupSubfolder { get; set; } = "backup";
    public ConflictPolicy ConflictPolicy { get; set; } = ConflictPolicy.UniqueSuffix;
}

public sealed class AutomationConfig
{
    public bool AutoRipOnInsert { get; set; }
    public int AutoRipDelaySeconds { get; set; } = 10;
    public bool EjectWhenDone { get; set; } = true;
    public bool EjectOnFailure { get; set; }
    public bool Notify { get; set; } = true;
    public bool PlaySound { get; set; } = true;
}

public sealed class ArchiveConfig
{
    /// <summary>Write SHA256SUMS for every produced file into the output folder.</summary>
    public bool Checksums { get; set; } = true;
    /// <summary>Write bromelia.json and the job log into the output folder.</summary>
    public bool ArchiveRecord { get; set; } = true;
}

public sealed class EpisodeConfig
{
    /// <summary>Split DVD "play all" titles of TV shows into one file per episode.</summary>
    public bool SplitPlayAll { get; set; } = true;
    /// <summary>Keep the unsplit title next to the episode files.</summary>
    public bool KeepPlayAll { get; set; } = true;
    /// <summary>Read episode numbers from the menus (needs ffmpeg and tesseract).</summary>
    public bool ReadMenuNumbers { get; set; } = true;
}

public sealed class PostProcessStep
{
    public Guid Id { get; set; } = Guid.NewGuid();
    public string Name { get; set; } = "Post-processing script";
    public bool Enabled { get; set; } = true;
    public string Executable { get; set; } = "";
    public string Interpreter { get; set; } = "";
    public string Arguments { get; set; } = "{outputDir}";
    public string WorkingDirectory { get; set; } = "";
    public RunCondition RunOn { get; set; } = RunCondition.Success;
    public bool PerFile { get; set; }
    public int TimeoutSeconds { get; set; }
    public Dictionary<string, string> Environment { get; set; } = new();
    public bool FailJobOnError { get; set; }
    /// <summary>Run only when the movie / show name or disc label matches this regular expression (empty = all).</summary>
    public string MatchName { get; set; } = "";
    /// <summary>Run only for these format codes (DVD, DVDe, BR, BRe, 4K, 4Ke; "BR*" = any code starting with BR). Empty = all.</summary>
    public List<string> MatchFormats { get; set; } = new();
}

public sealed class GeneratedProfile
{
    public string Name { get; set; } = "Bromelia";
    public string SelectionRule { get; set; } = "";
    public bool SetFirstAudioTrackAsDefault { get; set; } = true;
    public bool SetFirstSubtitleTrackAsDefault { get; set; } = true;
    public bool SetFirstForcedSubtitleTrackAsDefault { get; set; } = true;
    public bool IgnoreForcedSubtitlesFlag { get; set; } = true;
    [JsonPropertyName("useISO639Type2T")]
    public bool UseIso639Type2T { get; set; }
    public bool InsertFirstChapter00IfMissing { get; set; } = true;
    [JsonConverter(typeof(LpcmOutputConverter))]
    public LpcmOutput LpcmStereo { get; set; } = LpcmOutput.Lpcm;
    [JsonConverter(typeof(LpcmOutputConverter))]
    public LpcmOutput LpcmMultichannel { get; set; } = LpcmOutput.FlacBest;
}

public sealed class ProfileConfig
{
    public ProfileMode Mode { get; set; } = ProfileMode.MakemkvDefault;
    public string CustomPath { get; set; } = "";
    public GeneratedProfile Generated { get; set; } = new();
}

public sealed class DriveMatch
{
    /// <summary>Drive identification reported by MakeMKV (model, firmware, usually serial).</summary>
    public string DriveName { get; set; } = "";
    /// <summary>OS device (e.g. E: on Windows). Used when DriveName is empty.</summary>
    public string DevicePath { get; set; } = "";

    public bool Matches(DriveScanEntry e)
    {
        var name = DriveName.Trim();
        if (name.Length > 0) return Normalize(name) == Normalize(e.DriveName);
        return DevicePath.Length > 0 && string.Equals(DevicePath, e.DevicePath, StringComparison.OrdinalIgnoreCase);
    }

    public static string Normalize(string s) =>
        string.Join(' ', s.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries)).ToLowerInvariant();
}

public sealed class DriveConfig
{
    public Guid Id { get; set; } = Guid.NewGuid();
    public string Name { get; set; } = "Drive";
    public bool Enabled { get; set; } = true;
    public DriveMatch Match { get; set; } = new();
    /// <summary>MakeMKV setting overrides for this drive; a missing key inherits the global value.</summary>
    public Dictionary<string, string> Settings { get; set; } = new();
    public ProfileConfig Profile { get; set; } = new();
    public RipConfig Rip { get; set; } = new();
    public OutputConfig Output { get; set; } = new();
    public AutomationConfig Automation { get; set; } = new();
    public ArchiveConfig Archive { get; set; } = new();
    public EpisodeConfig Episodes { get; set; } = new();
    public List<PostProcessStep> PostProcess { get; set; } = new();

    /// <summary>Version 1 defaults kept MakeMKV's file names and named folders after the disc label; version 2
    /// names everything "{name} - … - {format}". Only untouched defaults are replaced.</summary>
    public void UpgradeNaming(int fromVersion)
    {
        if (fromVersion >= 2) return;
        if (string.IsNullOrWhiteSpace(Output.FileNameTemplate)) Output.FileNameTemplate = OutputConfig.DefaultFileNameTemplate;
        if (Output.FolderTemplate == OutputConfig.LegacyFolderTemplate) Output.FolderTemplate = OutputConfig.DefaultFolderTemplate;
    }

    public DriveConfig Clone() => JsonSerializer.Deserialize<DriveConfig>(JsonSerializer.Serialize(this, ConfigJson.Options), ConfigJson.Options)!;

    /// <summary>Copy of <paramref name="other"/> keeping this drive's identity (id, name, match, enabled).</summary>
    public DriveConfig ApplyingBody(DriveConfig other)
    {
        var c = other.Clone();
        c.Id = Id;
        c.Name = Name;
        c.Match = new DriveMatch { DriveName = Match.DriveName, DevicePath = Match.DevicePath };
        c.Enabled = Enabled;
        foreach (var s in c.PostProcess) s.Id = Guid.NewGuid();
        return c;
    }
}

public sealed class DrivePreset
{
    public Guid Id { get; set; } = Guid.NewGuid();
    public string Name { get; set; } = "";
    public DriveConfig Config { get; set; } = new();
}

public sealed class AppConfig
{
    public const int CurrentVersion = 2;

    public int Version { get; set; } = CurrentVersion;
    public string MakemkvconPath { get; set; } = "";
    public string MkvmergePath { get; set; } = "";
    public string OutputRoot { get; set; } = "";
    public int PollIntervalSeconds { get; set; } = 10;
    public bool PollWhileRipping { get; set; }
    public int MaxConcurrentJobs { get; set; }
    public string RegistrationKey { get; set; } = "";
    public Dictionary<string, string> GlobalSettings { get; set; } = new();
    public DriveConfig DefaultDrive { get; set; } = new() { Name = "Default" };
    public List<DriveConfig> Drives { get; set; } = new();
    public List<DrivePreset> Presets { get; set; } = new();
    /// <summary>Post-processing steps for every drive, usually limited with MatchName / MatchFormats ("plugins").
    /// They run after the drive's own steps.</summary>
    public List<PostProcessStep> Plugins { get; set; } = new();
    public int HistoryLimit { get; set; } = 500;

    /// <summary>Upgrades a configuration loaded from a file of version <paramref name="loaded"/>.</summary>
    public AppConfig Upgrade(int loaded)
    {
        if (loaded < 2)
        {
            DefaultDrive.UpgradeNaming(loaded);
            foreach (var d in Drives) d.UpgradeNaming(loaded);
            foreach (var p in Presets) p.Config.UpgradeNaming(loaded);
        }
        Version = CurrentVersion;
        return this;
    }

    public DriveConfig? DriveConfigFor(DriveScanEntry e) =>
        Drives.FirstOrDefault(d => d.Enabled && d.Match.Matches(e)) ?? Drives.FirstOrDefault(d => d.Match.Matches(e));

    public DriveConfig? DriveConfigById(Guid id) => DefaultDrive.Id == id ? DefaultDrive : Drives.FirstOrDefault(d => d.Id == id);

    public Dictionary<string, string> EffectiveSettings(DriveConfig drive)
    {
        var s = new Dictionary<string, string>(GlobalSettings);
        foreach (var kv in drive.Settings) s[kv.Key] = kv.Value;
        if (RegistrationKey.Length > 0) s["app_Key"] = RegistrationKey;
        return s;
    }

    public string OutputRootFor(DriveConfig drive) =>
        string.IsNullOrWhiteSpace(drive.Output.RootOverride) ? OutputRoot : drive.Output.RootOverride.Trim();
}

public static class ConfigJson
{
    public static readonly JsonSerializerOptions Options = Create();

    static JsonSerializerOptions Create()
    {
        var o = new JsonSerializerOptions
        {
            PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
            DictionaryKeyPolicy = null,
            WriteIndented = true,
            PropertyNameCaseInsensitive = true,
            ReadCommentHandling = JsonCommentHandling.Skip,
            AllowTrailingCommas = true,
            DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        };
        o.Converters.Add(new JsonStringEnumConverter(JsonNamingPolicy.CamelCase));
        return o;
    }

    public static AppConfig Parse(string json)
    {
        var config = JsonSerializer.Deserialize<AppConfig>(json, Options) ?? new AppConfig();
        // Files without a version are version 1.
        int loaded = 1;
        using (var doc = JsonDocument.Parse(json, new JsonDocumentOptions { CommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true }))
            if (doc.RootElement.ValueKind == JsonValueKind.Object)
                foreach (var p in doc.RootElement.EnumerateObject())
                    if (string.Equals(p.Name, "version", StringComparison.OrdinalIgnoreCase) && p.Value.TryGetInt32(out var v)) loaded = v;
        return config.Upgrade(loaded);
    }
    public static string Serialize<T>(T value) => JsonSerializer.Serialize(value, Options);
}
