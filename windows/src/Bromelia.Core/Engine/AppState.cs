using System.Collections.ObjectModel;
using System.Text.Json;
using Bromelia.Core.Config;
using Bromelia.Core.Logic;
using Bromelia.Core.Robot;

namespace Bromelia.Core.Engine;

/// <summary>The disc opened in the UI for one source: title listing and the user's choices.</summary>
public sealed class DiscSession : ObservableObject
{
    DiscInfo? _info;
    bool _isLoading;
    string? _loadError;
    double _progress;
    string _operation = "";
    Guid _configId;
    string _outputFolderOverride = "";
    string? _libreDrive;

    public DiscSession(string id, DiscSource source, Guid configId)
    {
        Id = id;
        Source = source;
        _configId = configId;
    }

    public string Id { get; }
    public DiscSource Source { get; set; }
    public Guid ConfigId { get => _configId; set => Set(ref _configId, value); }
    public DiscInfo? Info { get => _info; set { if (Set(ref _info, value)) OnPropertyChanged(nameof(HasInfo)); } }
    public bool HasInfo => _info != null;
    public bool IsLoading { get => _isLoading; set => Set(ref _isLoading, value); }
    public string? LoadError { get => _loadError; set => Set(ref _loadError, value); }
    public double Progress { get => _progress; set => Set(ref _progress, value); }
    public string Operation { get => _operation; set => Set(ref _operation, value); }
    public string OutputFolderOverride { get => _outputFolderOverride; set => Set(ref _outputFolderOverride, value); }
    /// <summary>What MakeMKV said about LibreDrive when the disc was opened (drives only), as a line for the disc page.</summary>
    public string? LibreDrive { get => _libreDrive; set => Set(ref _libreDrive, value); }
    public bool LibreDriveRequired { get; set; }
    /// <summary>Movie / show name for file names. Empty = inferred from the disc.</summary>
    public string MediaName { get; set; } = "";
    /// <summary>Null = decide automatically.</summary>
    public MediaKind? MediaKind { get; set; }
    /// <summary>First episode number on this disc. Null = read from the menus or 1.</summary>
    public int? FirstEpisode { get; set; }
    /// <summary>File system flags from the drive scan.</summary>
    public DiscFlags? DiscFlags { get; set; }
    public ObservableCollection<LogEntry> Log { get; } = new();
    public HashSet<int> SelectedTitles { get; } = new();
    /// <summary>Only titles whose tracks the user customised.</summary>
    public Dictionary<int, HashSet<int>> TrackSelections { get; } = new();
    public Dictionary<int, string> TitleNameOverrides { get; } = new();
    public ProcessRunner? Runner { get; set; }

    /// <summary>Raised when title / track selections change (so views can refresh summaries).</summary>
    public event Action? SelectionChanged;

    public void AppendLog(string text, Severity sev)
    {
        Log.Add(new LogEntry(DateTime.Now, sev, text));
        if (Log.Count > 5000) for (int i = 0; i < 1000; i++) Log.RemoveAt(0);
    }

    public void Reset()
    {
        Runner?.Cancel();
        Runner = null;
        Info = null;
        IsLoading = false;
        LoadError = null;
        SelectedTitles.Clear();
        TrackSelections.Clear();
        TitleNameOverrides.Clear();
        MediaName = "";
        MediaKind = null;
        FirstEpisode = null;
        LibreDrive = null;
        LibreDriveRequired = false;
        Progress = 0;
        Operation = "";
        SelectionChanged?.Invoke();
    }

    public void ApplyRule(TitleSelection rule)
    {
        if (Info == null) return;
        var r = JsonSerializer.Deserialize<TitleSelection>(JsonSerializer.Serialize(rule))!;
        if (r.Strategy == TitleStrategy.Manual) r.Strategy = TitleStrategy.All;
        SelectedTitles.Clear();
        foreach (var i in TitleSelector.Evaluate(Info.Titles, r).SelectedIndices) SelectedTitles.Add(i);
        TrackSelections.Clear();
        SelectionChanged?.Invoke();
    }

    public void SetTitleSelected(int title, bool selected)
    {
        if (selected) SelectedTitles.Add(title); else SelectedTitles.Remove(title);
        SelectionChanged?.Invoke();
    }

    public void NotifySelectionChanged() => SelectionChanged?.Invoke();

    public bool HasCustomTracks(int title) => TrackSelections.ContainsKey(title);

    public void CustomizeTracks(int title)
    {
        if (Info?.Title(title) is { } t) TrackSelections[title] = t.Tracks.Select(x => x.Index).ToHashSet();
        SelectionChanged?.Invoke();
    }

    public void ResetTracks(int title)
    {
        TrackSelections.Remove(title);
        SelectionChanged?.Invoke();
    }

    public bool IsTrackSelected(int title, int track) => TrackSelections.TryGetValue(title, out var s) && s.Contains(track);

    public void SetTrack(int title, int track, bool selected)
    {
        if (!TrackSelections.TryGetValue(title, out var set)) return;
        if (selected) set.Add(track); else set.Remove(track);
        SelectionChanged?.Invoke();
    }

    public long SelectedSizeBytes => Info?.Titles.Where(t => SelectedTitles.Contains(t.Index)).Sum(t => t.SizeBytes) ?? 0;
}

/// <summary>A drive as shown in the UI: MakeMKV's report merged with its configuration.</summary>
public sealed record DriveItem(string Id, string LaneKey, DriveScanEntry? Entry, DriveConfig? Config)
{
    public string DisplayName => Config?.Name ?? (Entry != null ? ShortModel(Entry.DriveName) : "Drive");
    public bool IsConnected => Entry?.IsPresent ?? false;

    public static string ShortModel(string name)
    {
        var parts = name.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        return parts.Length > 2 ? string.Join(" ", parts.Skip(1).Take(3)) : (name.Length == 0 ? "Drive" : name);
    }
}

/// <summary>
/// Central application state: configuration, drives, disc sessions, the job queue and history.
/// Create it on the UI thread; all members must be used from that thread.
/// </summary>
public sealed class AppState : ObservableObject
{
    readonly UiDispatcher _ui;
    readonly Dictionary<Guid, JobRunner> _runners = new();
    readonly Dictionary<string, DriveState> _knownStates = new();
    bool _firstScanDone;
    bool _isScanning;
    string _makemkvVersion = "";
    string? _lastError;
    DateTime? _lastScan;
    CancellationTokenSource? _saveCts;

    public AppState(IPlatformServices platform, string? configPath = null, SettingsCatalog? catalog = null)
    {
        _ui = new UiDispatcher();
        Platform = platform;
        ConfigPath = configPath ?? Paths.ConfigFile;
        Catalog = catalog ?? SettingsCatalog.Load();
        Config = ConfigStore.Load(ConfigPath);
        if (Config.OutputRoot.Length == 0) Config.OutputRoot = Paths.DefaultOutputRoot;
        foreach (var h in ConfigStore.LoadHistory()) History.Add(h);
    }

    public IPlatformServices Platform { get; }
    public string ConfigPath { get; }
    public SettingsCatalog Catalog { get; }
    public AppConfig Config { get; private set; }
    public ObservableCollection<DriveScanEntry> ScannedDrives { get; } = new();
    public Dictionary<string, DiscSession> Sessions { get; } = new();
    public ObservableCollection<DiscSession> FileSessions { get; } = new();
    public ObservableCollection<RipJob> Jobs { get; } = new();
    public ObservableCollection<HistoryRecord> History { get; } = new();
    public ObservableCollection<RobotMessage> ScanMessages { get; } = new();

    public bool IsScanning { get => _isScanning; private set => Set(ref _isScanning, value); }
    public string MakemkvVersion { get => _makemkvVersion; private set => Set(ref _makemkvVersion, value); }
    public string? LastError { get => _lastError; set => Set(ref _lastError, value); }
    MakeMKVNotice? _makemkvProblem;
    /// <summary>A problem with MakeMKV itself (expired key, outdated version) reported by the last makemkvcon run.</summary>
    public MakeMKVNotice? MakemkvProblem { get => _makemkvProblem; set => Set(ref _makemkvProblem, value); }
    public DateTime? LastScan { get => _lastScan; private set => Set(ref _lastScan, value); }

    /// <summary>Raised whenever the drive list or configurations change.</summary>
    public event Action? DrivesChanged;

    public string? Makemkvcon => Paths.ResolveTool(Config.MakemkvconPath, Paths.MakemkvconCandidates(), "makemkvcon");
    public string? Mkvmerge => Paths.ResolveTool(Config.MkvmergePath, Paths.MkvmergeCandidates(), "mkvmerge");
    IReadOnlyCollection<string> CatalogKeys => Catalog.AllSettings.Select(s => s.Key).ToList();

    // --- configuration -----------------------------------------------------------------------

    /// <summary>Call after changing Config; saves (debounced) and refreshes drive items.</summary>
    public void ConfigChanged()
    {
        EnsureSessions(ScannedDrives);
        DrivesChanged?.Invoke();
        _saveCts?.Cancel();
        var cts = _saveCts = new CancellationTokenSource();
        var snapshot = ConfigJson.Serialize(Config);
        _ = Task.Delay(400, cts.Token).ContinueWith(t =>
        {
            if (!t.IsCanceled) ConfigStore.SaveRaw(ConfigPath, snapshot);
        }, TaskScheduler.Default);
    }

    public void SaveNow()
    {
        _saveCts?.Cancel();
        ConfigStore.Save(ConfigPath, Config);
        ConfigStore.SaveHistory(History);
    }

    public void ReplaceConfig(AppConfig config)
    {
        Config = config;
        OnPropertyChanged(nameof(Config));
        ConfigChanged();
    }

    /// <summary>First run: import MakeMKV's own settings.</summary>
    public void ImportFromMakeMkvIfFirstRun()
    {
        if (File.Exists(ConfigPath)) return;
        ImportSettings(MakeMKVEnvironment.InstalledSettings());
        SaveNow();
    }

    public void ImportSettings(IReadOnlyDictionary<string, string> s)
    {
        var known = CatalogKeys.ToHashSet();
        foreach (var kv in s)
            if (known.Contains(kv.Key) && (MakeMKVEnvironment.UsesRegistry || kv.Key != "app_DataDir"))
                Config.GlobalSettings[kv.Key] = kv.Value;
        if (s.TryGetValue("app_DestinationDir", out var dest) && dest.Length > 0) Config.OutputRoot = dest;
        ConfigChanged();
    }

    public void UpdateDrive(DriveConfig c)
    {
        if (c.Id == Config.DefaultDrive.Id) Config.DefaultDrive = c;
        else
        {
            int i = Config.Drives.FindIndex(d => d.Id == c.Id);
            if (i >= 0) Config.Drives[i] = c; else Config.Drives.Add(c);
        }
        ConfigChanged();
    }

    public void RemoveDriveConfig(Guid id)
    {
        Config.Drives.RemoveAll(d => d.Id == id);
        ConfigChanged();
    }

    public DriveConfig Configure(DriveScanEntry e)
    {
        if (Config.DriveConfigFor(e) is { } existing) return existing;
        var c = Config.DefaultDrive.Clone();
        c.Id = Guid.NewGuid();
        foreach (var s in c.PostProcess) s.Id = Guid.NewGuid();
        c.Name = DriveItem.ShortModel(e.DriveName);
        c.Match = new DriveMatch { DriveName = e.DriveName, DevicePath = e.DevicePath };
        Config.Drives.Add(c);
        ConfigChanged();
        return c;
    }

    public void SavePreset(string name, DriveConfig from)
    {
        var body = from.Clone();
        body.Match = new DriveMatch();
        Config.Presets.Add(new DrivePreset { Name = name, Config = body });
        ConfigChanged();
    }

    // --- drives -------------------------------------------------------------------------------

    public List<DriveItem> DriveItems
    {
        get
        {
            var items = new List<DriveItem>();
            var used = new HashSet<Guid>();
            foreach (var e in ScannedDrives.Where(d => d.IsPresent))
            {
                var c = Config.DriveConfigFor(e);
                if (c != null) used.Add(c.Id);
                items.Add(new DriveItem(c?.Id.ToString() ?? e.LaneKey, e.LaneKey, e, c));
            }
            foreach (var c in Config.Drives.Where(c => !used.Contains(c.Id)))
                items.Add(new DriveItem(c.Id.ToString(), "cfg:" + c.Id, null, c));
            return items;
        }
    }

    public DriveItem? DriveItemById(string id) => DriveItems.FirstOrDefault(d => d.Id == id);
    public DiscSession? SessionFor(DriveItem item) => item.Entry == null ? null : Sessions.GetValueOrDefault(item.LaneKey);
    public DriveScanEntry? EntryForLane(string lane) => ScannedDrives.FirstOrDefault(e => e.LaneKey == lane);
    public DriveConfig ConfigForSession(DiscSession s) => Config.DriveConfigById(s.ConfigId) ?? Config.DefaultDrive;

    public async Task RefreshDrivesAsync(bool force)
    {
        if (IsScanning) return;
        var exe = Makemkvcon;
        if (exe == null)
        {
            LastError = "makemkvcon was not found. Install MakeMKV or set its location in Settings.";
            return;
        }
        bool busy = Jobs.Any(j => j.State == JobState.Running) || Sessions.Values.Any(s => s.IsLoading);
        if (busy && !force && !Config.PollWhileRipping) return;
        IsScanning = true;
        try
        {
            var env = MakeMKVEnvironment.Prepare(exe, Config, Config.DefaultDrive, Path.Combine(Paths.AppData, "scanner"), CatalogKeys);
            var lines = new LineCollector();
            await env.RunAsync(MakeMKVEnvironment.ScanArguments(), lines.Add, null, TimeSpan.FromMinutes(3));
            var entries = new List<DriveScanEntry>();
            ScanMessages.Clear();
            foreach (var line in lines.All)
            {
                switch (RobotParser.Parse(line))
                {
                    case RobotEvent.Drive d: entries.Add(d.Entry); break;
                    case RobotEvent.Message { Value: var m }:
                        if (m.Code == 1005 && m.Parameters.Count > 0) MakemkvVersion = m.Parameters[0];
                        if (MakeMKVNotice.From(m) is { IsLicenseProblem: true } problem) MakemkvProblem = problem;
                        if (m.Severity != Severity.Debug && m.Code is not (5010 or 5042 or 1005 or 1004)) ScanMessages.Add(m);
                        break;
                }
            }
            LastError = null;
            ApplyScan(entries);
        }
        catch (Exception e)
        {
            LastError = $"Drive scan failed: {e.Message}";
        }
        finally
        {
            IsScanning = false;
            LastScan = DateTime.Now;
        }
    }

    public void ApplyScan(IEnumerable<DriveScanEntry> entries)
    {
        var merged = new List<DriveScanEntry>();
        foreach (var e in entries.Where(e => e.IsPresent))
        {
            var busy = Jobs.Any(j => j.LaneKey == e.LaneKey && j.State == JobState.Running);
            var old = ScannedDrives.FirstOrDefault(o => o.LaneKey == e.LaneKey);
            merged.Add(busy && old != null ? old : e);
        }
        ScannedDrives.Clear();
        foreach (var e in merged) ScannedDrives.Add(e);
        EnsureSessions(merged);

        foreach (var e in merged)
        {
            _knownStates.TryGetValue(e.LaneKey, out var previous);
            bool hadPrevious = _knownStates.ContainsKey(e.LaneKey);
            _knownStates[e.LaneKey] = e.State;
            if (!_firstScanDone) continue;
            if (e.State == DriveState.Inserted && (!hadPrevious || previous != DriveState.Inserted)) DiscInserted(e);
            else if (e.State != DriveState.Inserted && previous == DriveState.Inserted) Sessions.GetValueOrDefault(e.LaneKey)?.Reset();
        }
        _firstScanDone = true;
        DrivesChanged?.Invoke();
    }

    void EnsureSessions(IEnumerable<DriveScanEntry> entries)
    {
        foreach (var e in entries)
        {
            var cfgId = Config.DriveConfigFor(e)?.Id ?? Config.DefaultDrive.Id;
            if (Sessions.TryGetValue(e.LaneKey, out var s))
            {
                s.Source = new DiscSource.Drive(e.Index, e.DevicePath);
                s.ConfigId = cfgId;
                s.DiscFlags = e.Flags;
            }
            else Sessions[e.LaneKey] = new DiscSession(e.LaneKey, new DiscSource.Drive(e.Index, e.DevicePath), cfgId) { DiscFlags = e.Flags };
        }
    }

    void DiscInserted(DriveScanEntry e)
    {
        Sessions.GetValueOrDefault(e.LaneKey)?.Reset();
        if (Config.DriveConfigFor(e) is not { Enabled: true, Automation.AutoRipOnInsert: true } cfg) return;
        if (Jobs.Any(j => j.LaneKey == e.LaneKey && !j.State.IsFinished())) return;
        var job = MakeJob(e, cfg, cfg.Rip.Mode);
        job.IsAutomatic = true;
        job.StartAt = DateTime.Now.AddSeconds(Math.Max(0, cfg.Automation.AutoRipDelaySeconds));
        job.State = JobState.Waiting;
        job.Phase = "Automatic rip";
        Enqueue(job);
    }

    public async Task EjectAsync(string lane)
    {
        if (EntryForLane(lane) is not { DevicePath.Length: > 0 } e) return;
        Sessions.GetValueOrDefault(lane)?.Reset();
        if (!await Platform.EjectAsync(e.DevicePath)) LastError = $"Could not eject {e.DevicePath}";
        await Task.Delay(3000);
        await RefreshDrivesAsync(true);
    }

    // --- disc sessions ------------------------------------------------------------------------

    public DiscSession OpenFileSource(string path)
    {
        // A disc image, a disc folder, or the disc folder a file (.IFO, .mpls, .m2ts, …) belongs to.
        var source = SourceResolver.Resolve(path, Directory.Exists(path));
        var key = source.InfoArgument;
        if (!Sessions.TryGetValue(key, out var s))
        {
            s = new DiscSession(key, source, Config.DefaultDrive.Id);
            if (source is DiscSource.Folder f && !string.Equals(f.Path, path.TrimEnd('\\', '/'), StringComparison.OrdinalIgnoreCase))
                s.AppendLog($"Opening the disc that {Path.GetFileName(path)} belongs to: {f.Path}", Severity.Info);
            Sessions[key] = s;
            FileSessions.Add(s);
        }
        _ = LoadDiscAsync(s);
        return s;
    }

    public void CloseFileSource(DiscSession s)
    {
        s.Reset();
        Sessions.Remove(s.Id);
        FileSessions.Remove(s);
    }

    public async Task LoadDiscAsync(DiscSession s)
    {
        if (s.IsLoading) return;
        var exe = Makemkvcon;
        if (exe == null) { s.LoadError = "makemkvcon not found"; return; }
        if (s.Source is DiscSource.Drive && Jobs.Any(j => j.LaneKey == s.Id && j.State == JobState.Running))
        {
            s.LoadError = "The drive is busy with a job.";
            return;
        }
        s.Reset();
        s.IsLoading = true;
        s.Operation = "Opening disc";
        var cfg = ConfigForSession(s);
        try
        {
            var home = Path.Combine(Paths.AppData, "sessions", Convert.ToHexString(System.Security.Cryptography.SHA1.HashData(System.Text.Encoding.UTF8.GetBytes(s.Id)))[..12]);
            var env = MakeMKVEnvironment.Prepare(exe, Config, cfg, home, CatalogKeys);
            var builder = new DiscInfoBuilder();
            var errors = new List<string>();
            bool showDebug = env.Settings.TryGetValue("app_ShowDebug", out var d) && d == "1";
            var result = await env.RunAsync(env.InfoArguments(s.Source, cfg.Rip), line =>
            {
                if (RobotParser.Parse(line) is not { } ev) return;
                _ui.Post(() =>
                {
                    builder.Consume(ev);
                    switch (ev)
                    {
                        case RobotEvent.Message { Value: var m }:
                            switch (MakeMKVNotice.From(m))
                            {
                                case { Kind: NoticeKind.LibreDrive } n: s.LibreDrive = $"LibreDrive: enabled{(n.Detail.Length > 0 ? $" ({n.Detail})" : "")}"; break;
                                case { Kind: NoticeKind.LibreDriveRequired }:
                                    s.LibreDrive = "LibreDrive required: this drive can't decrypt this disc";
                                    s.LibreDriveRequired = true;
                                    break;
                                case { IsLicenseProblem: true } n: MakemkvProblem = n; break;
                            }
                            if (m.Severity == Severity.Debug && !showDebug) break;
                            s.AppendLog(m.Text, m.Severity);
                            if (m.Severity == Severity.Error) errors.Add(m.Text);
                            break;
                        case RobotEvent.ProgressCurrentTitle c: s.Operation = c.Name; break;
                        case RobotEvent.ProgressValue v when v.Max > 0: s.Progress = (double)v.Total / v.Max; break;
                        case RobotEvent.Raw r: s.AppendLog(r.Line, Severity.Info); break;
                    }
                });
            }, r => { s.Runner = r; s.AppendLog("$ " + r.CommandLine, Severity.Info); });
            await _ui.Barrier();
            s.Runner = null;
            s.IsLoading = false;
            if (s.Source is DiscSource.Drive && s.LibreDrive == null) s.LibreDrive = "LibreDrive: not in use for this disc";
            if (result.Cancelled) { s.LoadError = "Cancelled"; return; }
            if (builder.Info.Titles.Count == 0)
            {
                if (MakemkvProblem is { IsLicenseProblem: true } p) s.LoadError = p.Explanation;
                else if (s.Source is DiscSource.Folder && errors.Count == 0)
                    s.LoadError = "MakeMKV found no titles here. It opens disc images and disc folders (with BDMV, VIDEO_TS or HVDVD_TS), not single video files.";
                else s.LoadError = errors.LastOrDefault() ?? $"No titles found (exit status {result.ExitCode}).";
                return;
            }
            s.Info = builder.Info;
            s.ApplyRule(cfg.Rip.TitleSelection);
        }
        catch (Exception e)
        {
            s.IsLoading = false;
            s.LoadError = e.Message;
        }
    }

    // --- jobs ---------------------------------------------------------------------------------

    public RipJob MakeJob(DriveScanEntry e, DriveConfig cfg, RipMode mode) =>
        new(new DiscSource.Drive(e.Index, e.DevicePath), cfg.Clone(), e.LaneKey, cfg.Name, e.DiscName, mode) { DiscFlags = e.Flags };

    static void ApplyIdentityChoices(DiscSession s, RipJob job)
    {
        job.MediaName = s.MediaName.Trim();
        job.MediaKind = s.MediaKind;
        job.FirstEpisode = s.FirstEpisode;
        job.DiscFlags ??= s.DiscFlags;
    }

    public void QuickRip(DriveItem item, RipMode? mode = null)
    {
        if (item.Entry == null) return;
        var cfg = item.Config ?? Config.DefaultDrive;
        var job = MakeJob(item.Entry, cfg, mode ?? cfg.Rip.Mode);
        if (Sessions.GetValueOrDefault(item.LaneKey) is { Info: { } info } session)
        {
            job.PreloadedInfo = info;
            job.DiscLabel = info.Name;
            ApplyIdentityChoices(session, job);
        }
        Enqueue(job);
    }

    public void RipSession(DiscSession s, RipMode mode)
    {
        var cfg = ConfigForSession(s).Clone();
        var label = s.Source is DiscSource.Drive ? cfg.Name : s.Source.DisplayName;
        var job = new RipJob(s.Source, cfg, s.Id, label, s.Info?.Name ?? "", mode) { PreloadedInfo = s.Info };
        ApplyIdentityChoices(s, job);
        if (mode.MakesMkv())
        {
            job.ManualTitles = s.SelectedTitles.OrderBy(i => i).ToList();
            job.TrackSelections = s.TrackSelections.Where(kv => s.SelectedTitles.Contains(kv.Key)).ToDictionary(kv => kv.Key, kv => new HashSet<int>(kv.Value));
            job.TitleNameOverrides = s.TitleNameOverrides.Where(kv => s.SelectedTitles.Contains(kv.Key) && kv.Value.Trim().Length > 0).ToDictionary(kv => kv.Key, kv => kv.Value);
        }
        if (s.OutputFolderOverride.Trim().Length > 0)
        {
            cfg.Output.RootOverride = s.OutputFolderOverride.Trim();
            cfg.Output.FolderTemplate = "";
            cfg.Output.ConflictPolicy = ConflictPolicy.Overwrite;
        }
        Enqueue(job);
    }

    public void Enqueue(RipJob job)
    {
        Jobs.Add(job);
        Pump();
    }

    public void Cancel(RipJob job)
    {
        if (_runners.TryGetValue(job.Id, out var r)) r.Cancel();
        else if (!job.State.IsFinished())
        {
            job.State = JobState.Cancelled;
            job.Phase = "Cancelled";
            job.FinishedAt = DateTime.Now;
            RecordHistory(job);
        }
    }

    public void StartNow(RipJob job)
    {
        if (job.State != JobState.Waiting) return;
        job.StartAt = null;
        job.State = JobState.Queued;
        Pump();
    }

    public void Retry(RipJob job)
    {
        var j = new RipJob(job.Source, job.Drive, job.LaneKey, job.SourceLabel, job.DiscLabel, job.Mode)
        {
            PreloadedInfo = job.PreloadedInfo,
            ManualTitles = job.ManualTitles,
            TrackSelections = job.TrackSelections,
            TitleNameOverrides = job.TitleNameOverrides,
            MediaName = job.MediaName,
            MediaKind = job.MediaKind,
            FirstEpisode = job.FirstEpisode,
            DiscFlags = job.DiscFlags,
        };
        Enqueue(j);
    }

    public void ClearFinishedJobs()
    {
        foreach (var j in Jobs.Where(j => j.State.IsFinished()).ToList()) Jobs.Remove(j);
    }

    public void MoveJob(RipJob job, int offset)
    {
        int i = Jobs.IndexOf(job), k = i + offset;
        if (i < 0 || k < 0 || k >= Jobs.Count) return;
        Jobs.Move(i, k);
    }

    public int ActiveJobCount => Jobs.Count(j => j.State == JobState.Running);

    public RipJob? ActiveJob(string lane) =>
        Jobs.FirstOrDefault(j => j.LaneKey == lane && j.State is JobState.Running or JobState.Waiting or JobState.Queued);

    /// <summary>Starts queued jobs whose lane is free. Call periodically (once a second).</summary>
    public void Pump()
    {
        var now = DateTime.Now;
        foreach (var j in Jobs.Where(j => j.State == JobState.Waiting && j.StartAt is { } t && t <= now).ToList()) j.State = JobState.Queued;
        foreach (var j in Jobs.Where(j => j.State == JobState.Running)) j.Tick();
        foreach (var job in Jobs.Where(j => j.State == JobState.Queued).ToList())
        {
            if (Config.MaxConcurrentJobs > 0 && ActiveJobCount >= Config.MaxConcurrentJobs) break;
            if (Jobs.Any(j => j.LaneKey == job.LaneKey && j.State == JobState.Running)) continue;
            if (Sessions.GetValueOrDefault(job.LaneKey) is { IsLoading: true }) continue;
            Start(job);
        }
    }

    void Start(RipJob job)
    {
        var exe = Makemkvcon;
        if (exe == null)
        {
            job.State = JobState.Failed;
            job.ErrorMessage = "makemkvcon not found";
            job.FinishedAt = DateTime.Now;
            RecordHistory(job);
            return;
        }
        if (job.Source is DiscSource.Drive { DevicePath.Length: > 0 } d && ScannedDrives.FirstOrDefault(e => e.DevicePath == d.DevicePath) is { } fresh)
            job.Source = new DiscSource.Drive(fresh.Index, d.DevicePath);
        var runner = new JobRunner(job, Config, exe, Mkvmerge, _ui, Platform, CatalogKeys);
        runner.Finished += finished =>
        {
            _runners.Remove(finished.Id);
            UpdateKeepAwake();
            if (finished.MakemkvProblem is { IsLicenseProblem: true } problem) MakemkvProblem = problem;
            RecordHistory(finished);
            _ = Task.Delay(3000).ContinueWith(_ => _ui.Post(() => _ = RefreshDrivesAsync(true)), TaskScheduler.Default);
            Pump();
        };
        _runners[job.Id] = runner;
        UpdateKeepAwake();
        _ = runner.RunAsync();
    }

    bool _keepingAwake;

    /// <summary>Keeps the computer awake while any job runs (when enabled in the settings).</summary>
    public void UpdateKeepAwake()
    {
        bool busy = _runners.Count > 0 && Config.PreventSleep;
        if (busy == _keepingAwake) return;
        _keepingAwake = busy;
        Platform.KeepAwake(busy);
    }

    void RecordHistory(RipJob job)
    {
        var rec = new HistoryRecord
        {
            Id = job.Id, Title = job.Title, DriveName = job.Drive.Name, DiscName = job.DiscLabel, Mode = job.Mode, State = job.State,
            StartedAt = job.StartedAt, FinishedAt = job.FinishedAt, OutputDirectory = job.OutputDirectory, Files = job.ProducedFiles.ToList(),
            ErrorMessage = job.ErrorMessage, LogPath = job.LogFile, Warnings = job.WarningCount, Errors = job.ErrorCount,
        };
        foreach (var old in History.Where(h => h.Id == rec.Id).ToList()) History.Remove(old);
        History.Insert(0, rec);
        while (History.Count > Config.HistoryLimit)
        {
            var last = History[^1];
            try { Directory.Delete(Path.GetDirectoryName(last.LogPath)!, true); } catch (IOException) { } catch (UnauthorizedAccessException) { }
            History.RemoveAt(History.Count - 1);
        }
        ConfigStore.SaveHistory(History);
    }

    public void ClearHistory()
    {
        foreach (var h in History)
        {
            try { Directory.Delete(Path.GetDirectoryName(h.LogPath)!, true); } catch (IOException) { } catch (UnauthorizedAccessException) { }
        }
        History.Clear();
        ConfigStore.SaveHistory(History);
    }

    /// <summary>Downloads the current beta key from the MakeMKV forum and registers it with MakeMKV.</summary>
    public async Task<string> InstallBetaKeyAsync()
    {
        string key;
        try { key = await BetaKey.FetchAsync(); }
        catch (Exception e) { return $"Could not get the beta key: {e.Message}"; }
        var result = await RegisterWithMakeMkvAsync(key);
        // A key typed into Bromelia would override the new one for every drive.
        if (Config.RegistrationKey.Length > 0) { Config.RegistrationKey = key; ConfigChanged(); }
        if (result.Contains("fail", StringComparison.OrdinalIgnoreCase)) return result;
        MakemkvProblem = null;
        return $"Registered the current beta key ({key[..Math.Min(8, key.Length)]}…). {result}";
    }

    public async Task<string> RegisterWithMakeMkvAsync(string key)
    {
        if (Makemkvcon is not { } exe) return "makemkvcon not found";
        var lines = new LineCollector();
        try
        {
            var r = await new ProcessRunner(exe, new[] { "reg", key }).RunAsync(lines.Add, TimeSpan.FromMinutes(1));
            var text = string.Join("\n", lines.All.Select(l => RobotParser.Parse(l) is RobotEvent.Message { Value: var m } ? m.Text : l));
            return r.ExitCode == 0 ? (text.Length == 0 ? "Key registered." : text) : $"Registration failed: {text}";
        }
        catch (Exception e) { return e.Message; }
    }
}

public static class ConfigStore
{
    public static AppConfig Load(string path)
    {
        if (!File.Exists(path)) return new AppConfig();
        try { return ConfigJson.Parse(File.ReadAllText(path)); }
        catch (Exception)
        {
            try { File.Copy(path, Path.ChangeExtension(path, $".broken-{DateTimeOffset.Now.ToUnixTimeSeconds()}.json"), true); } catch (IOException) { }
            return new AppConfig();
        }
    }

    public static void Save(string path, AppConfig config) => SaveRaw(path, ConfigJson.Serialize(config));

    public static void SaveRaw(string path, string json)
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            var tmp = path + ".tmp";
            File.WriteAllText(tmp, json);
            File.Move(tmp, path, overwrite: true);
        }
        catch (IOException) { }
    }

    public static List<HistoryRecord> LoadHistory()
    {
        try { return File.Exists(Paths.HistoryFile) ? JsonSerializer.Deserialize<List<HistoryRecord>>(File.ReadAllText(Paths.HistoryFile), ConfigJson.Options) ?? new() : new(); }
        catch (Exception) { return new(); }
    }

    public static void SaveHistory(IEnumerable<HistoryRecord> h) => SaveRaw(Paths.HistoryFile, ConfigJson.Serialize(h.ToList()));

    public sealed class ExportBundle
    {
        public string Format { get; set; } = "bromelia-drives";
        public int Version { get; set; } = 1;
        public List<DriveConfig> Drives { get; set; } = new();
        public List<DrivePreset> Presets { get; set; } = new();
    }
}
