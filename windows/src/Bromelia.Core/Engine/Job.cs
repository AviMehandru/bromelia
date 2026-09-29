using System.Collections.ObjectModel;
using System.Globalization;
using Bromelia.Core.Config;
using Bromelia.Core.Robot;

namespace Bromelia.Core.Engine;

/// <summary><see cref="CompletedWithErrors"/>: every title was saved, but MakeMKV reported read errors while ripping,
/// so the files may contain damaged or skipped data. They are kept apart from finished archives.</summary>
public enum JobState { Queued, Waiting, Running, Succeeded, CompletedWithErrors, Failed, Cancelled }

public static class JobStateExtensions
{
    public static bool IsFinished(this JobState s) => s is JobState.Succeeded or JobState.CompletedWithErrors or JobState.Failed or JobState.Cancelled;

    public static string Label(this JobState s) => s switch
    {
        JobState.Queued => "Queued",
        JobState.Waiting => "Starting soon",
        JobState.Running => "Running",
        JobState.Succeeded => "Completed",
        JobState.CompletedWithErrors => "Completed with read errors",
        JobState.Failed => "Failed",
        _ => "Cancelled",
    };

    public static string StatusWord(this JobState s) => s switch
    {
        JobState.Succeeded => "success",
        JobState.CompletedWithErrors => "errors",
        JobState.Cancelled => "cancelled",
        _ => "failed",
    };
}

public sealed record LogEntry(DateTime Time, Severity Severity, string Text)
{
    public string TimeText => Time.ToString("HH:mm:ss", CultureInfo.InvariantCulture);
}

/// <summary>A rip / backup / scan job. Mutated only on the UI thread.</summary>
public sealed class RipJob : ObservableObject
{
    public const int MaxLogEntries = 20_000;

    public Guid Id { get; } = Guid.NewGuid();
    public DateTime CreatedAt { get; } = DateTime.Now;
    public DiscSource Source { get; set; }
    /// <summary>Snapshot of the configuration when the job was created.</summary>
    public DriveConfig Drive { get; }
    public string LaneKey { get; }
    public string SourceLabel { get; }
    public RipMode Mode { get; set; }
    public DiscInfo? PreloadedInfo { get; set; }
    public List<int>? ManualTitles { get; set; }
    public Dictionary<int, HashSet<int>> TrackSelections { get; set; } = new();
    public Dictionary<int, string> TitleNameOverrides { get; set; } = new();
    /// <summary>Movie / show name typed by the user. Empty = inferred from the disc.</summary>
    public string MediaName { get; set; } = "";
    /// <summary>Movie or TV show as chosen by the user. Null = inferred.</summary>
    public Logic.MediaKind? MediaKind { get; set; }
    /// <summary>First episode number entered by the user. Null = read from the menus or 1.</summary>
    public int? FirstEpisode { get; set; }
    /// <summary>File system flags from the drive scan (used when the listing is unavailable).</summary>
    public DiscFlags? DiscFlags { get; set; }
    public bool IsAutomatic { get; set; }
    /// <summary>The history's successful jobs with their disc fingerprints, to recognise a disc archived before (set when it starts).</summary>
    public List<ArchivedCandidate> ArchivedCandidates { get; set; } = new();
    /// <summary>The disc's fingerprint (<see cref="DiscFingerprint"/>), once the job has read the listing.</summary>
    public string? Fingerprint { get; set; }
    public DateTime? StartAt { get; set; }

    JobState _state = JobState.Queued;
    string _phase = "Queued", _currentOperation = "", _totalOperation = "", _discLabel;
    double _currentProgress, _totalProgress;
    int _stepIndex, _stepCount = 1, _warnings, _errors;
    DateTime? _startedAt, _finishedAt;
    string? _outputDirectory, _errorMessage;
    StreamWriter? _logWriter;

    public RipJob(DiscSource source, DriveConfig drive, string laneKey, string sourceLabel, string discLabel, RipMode mode)
    {
        Source = source;
        Drive = drive;
        LaneKey = laneKey;
        SourceLabel = sourceLabel;
        _discLabel = discLabel;
        Mode = mode;
    }

    public JobState State { get => _state; set { if (Set(ref _state, value)) { OnPropertyChanged(nameof(StateLabel)); OnPropertyChanged(nameof(IsActive)); OnPropertyChanged(nameof(IsFinished)); } } }
    public string StateLabel => _state.Label();
    public bool IsActive => _state == JobState.Running;
    public bool IsFinished => _state.IsFinished();
    public string Phase { get => _phase; set => Set(ref _phase, value); }
    public string CurrentOperation { get => _currentOperation; set => Set(ref _currentOperation, value); }
    public string TotalOperation { get => _totalOperation; set => Set(ref _totalOperation, value); }
    public string DiscLabel { get => _discLabel; set { if (Set(ref _discLabel, value)) OnPropertyChanged(nameof(Title)); } }
    public double CurrentProgress { get => _currentProgress; set => Set(ref _currentProgress, value); }
    public double TotalProgress { get => _totalProgress; set { if (Set(ref _totalProgress, value)) OnPropertyChanged(nameof(OverallProgress)); } }
    public int StepIndex { get => _stepIndex; set { if (Set(ref _stepIndex, value)) OnPropertyChanged(nameof(OverallProgress)); } }
    public int StepCount { get => _stepCount; set { if (Set(ref _stepCount, value)) OnPropertyChanged(nameof(OverallProgress)); } }
    public DateTime? StartedAt { get => _startedAt; set => Set(ref _startedAt, value); }
    public DateTime? FinishedAt { get => _finishedAt; set => Set(ref _finishedAt, value); }
    public string? OutputDirectory { get => _outputDirectory; set => Set(ref _outputDirectory, value); }
    public string? ErrorMessage { get => _errorMessage; set => Set(ref _errorMessage, value); }
    public int WarningCount { get => _warnings; private set => Set(ref _warnings, value); }
    public int ErrorCount { get => _errors; private set => Set(ref _errors, value); }
    public ObservableCollection<string> ProducedFiles { get; } = new();
    public ObservableCollection<LogEntry> Log { get; } = new();
    public List<string> Commands { get; } = new();
    public List<int> RipTitles { get; set; } = new();
    public DiscInfo? DiscInfo { get; set; }
    public Logic.MediaIdentity? Identity { get; set; }
    public List<ArchiveRecord.EpisodeEntry> Episodes { get; } = new();
    public List<Checksums.Entry> Checksums { get; set; } = new();
    public string? ChecksumFile { get; set; }
    public string MakemkvVersion { get; set; } = "";
    public List<string> ErrorMessages { get; } = new();
    /// <summary>Errors MakeMKV reported while ripping or backing up (read errors, hash check failures, ...).</summary>
    public List<string> DataErrors { get; } = new();
    /// <summary>"Using LibreDrive mode (…)" details, when the drive read the disc in LibreDrive mode.</summary>
    public string? LibreDrive { get; set; }
    /// <summary>A problem with MakeMKV or the drive that explains a failure (expired key, LibreDrive required, …).</summary>
    public MakeMKVNotice? MakemkvProblem { get; set; }
    /// <summary>The listing the titles were ripped from (the disc's, a backup's, or a one-pass listing).</summary>
    public DiscInfo? RipInfo { get; set; }
    /// <summary>Started with the drive's configured mode (automatic and quick rips), so the mode may follow the disc's format.</summary>
    public bool UsesConfiguredMode { get; set; }
    /// <summary>The movie / show found online.</summary>
    public MediaMatch? Metadata { get; set; }
    /// <summary>Post-processing steps that run after the job, in the background queue.</summary>
    public BackgroundWork? Background { get; set; }

    public string Title => $"{(DiscLabel.Length == 0 ? "Disc" : DiscLabel)} — {SourceLabel}";

    public double OverallProgress
    {
        get
        {
            if (_state == JobState.Succeeded) return 1;
            var v = (StepIndex + Math.Clamp(TotalProgress, 0, 1)) / Math.Max(StepCount, 1);
            return Math.Clamp(v, 0, 1);
        }
    }

    public TimeSpan Elapsed => StartedAt is { } s ? (FinishedAt ?? DateTime.Now) - s : TimeSpan.Zero;

    public TimeSpan? EstimatedRemaining
    {
        get
        {
            var p = OverallProgress;
            if (_state != JobState.Running || p < 0.02 || Elapsed.TotalSeconds < 10) return null;
            return TimeSpan.FromSeconds(Elapsed.TotalSeconds / p * (1 - p));
        }
    }

    public string LogDirectory => Path.Combine(Paths.JobsDirectory, Id.ToString());
    public string LogFile => Path.Combine(LogDirectory, "log.txt");
    public string ManifestFile => Path.Combine(LogDirectory, "manifest.json");

    public void OpenLogFile()
    {
        Directory.CreateDirectory(LogDirectory);
        _logWriter = new StreamWriter(LogFile, append: true) { AutoFlush = true };
    }

    public void CloseLogFile()
    {
        _logWriter?.Dispose();
        _logWriter = null;
    }

    public void AppendLog(string text, Severity severity = Severity.Info)
    {
        var e = new LogEntry(DateTime.Now, severity, text);
        Log.Add(e);
        if (Log.Count > MaxLogEntries + 2000) while (Log.Count > MaxLogEntries) Log.RemoveAt(0);
        if (severity == Severity.Warning) WarningCount++;
        if (severity == Severity.Error) ErrorCount++;
        var tag = severity == Severity.Info ? "" : $"[{severity.ToString().ToUpperInvariant()}] ";
        _logWriter?.WriteLine($"{e.Time.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture)} {tag}{text}");
    }

    /// <summary>Raises change notifications for time-based values (elapsed / remaining).</summary>
    public void Tick()
    {
        OnPropertyChanged(nameof(Elapsed));
        OnPropertyChanged(nameof(EstimatedRemaining));
    }
}

public sealed class HistoryRecord
{
    public Guid Id { get; set; }
    public string Title { get; set; } = "";
    public string DriveName { get; set; } = "";
    public string DiscName { get; set; } = "";
    public RipMode Mode { get; set; }
    public JobState State { get; set; }
    public DateTime? StartedAt { get; set; }
    public DateTime? FinishedAt { get; set; }
    public string? OutputDirectory { get; set; }
    public List<string> Files { get; set; } = new();
    public string? ErrorMessage { get; set; }
    public string LogPath { get; set; } = "";
    public int Warnings { get; set; }
    public int Errors { get; set; }
    /// <summary>The disc's fingerprint (see "already archived"), or null.</summary>
    public string? Fingerprint { get; set; }
}

public sealed class JobManifest
{
    public sealed class TitleEntry
    {
        public int Index { get; set; }
        public string Name { get; set; } = "";
        public string Duration { get; set; } = "";
        public int Chapters { get; set; }
        public int? SourceTitleId { get; set; }
    }

    public string JobId { get; set; } = "";
    public string Status { get; set; } = "";
    public string Mode { get; set; } = "";
    public string DriveName { get; set; } = "";
    public string DriveId { get; set; } = "";
    public string DevicePath { get; set; } = "";
    public string Source { get; set; } = "";
    public string DiscName { get; set; } = "";
    public string DiscType { get; set; } = "";
    public string OutputDirectory { get; set; } = "";
    public List<string> Files { get; set; } = new();
    public List<TitleEntry> Titles { get; set; } = new();
    public string Name { get; set; } = "";
    public string Kind { get; set; } = "";
    public string Format { get; set; } = "";
    public string FormatCode { get; set; } = "";
    public bool Encrypted { get; set; }
    public int? Season { get; set; }
    public int? DiscNumber { get; set; }
    public List<ArchiveRecord.EpisodeEntry> Episodes { get; set; } = new();
    public string? ChecksumFile { get; set; }
    public List<Checksums.Entry> Checksums { get; set; } = new();
    public DateTime? StartedAt { get; set; }
    public DateTime? FinishedAt { get; set; }
    public string? Error { get; set; }
}

/// <summary>Post-processing steps to run after a job has finished (and the disc is out), in the background queue.</summary>
public sealed record BackgroundWork(Guid JobId, string Title, List<PostProcessStep> Steps, PostProcessor.Context Context, string LogFile);
