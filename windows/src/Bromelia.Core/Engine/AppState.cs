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
    /// <summary>First episode number on this disc. Null = read from the menus, continued from the previous disc, or 1.</summary>
    public int? FirstEpisode { get; set; }
    /// <summary>Release year, to find the right movie or show online. Null = none.</summary>
    public int? MediaYear { get; set; }
    /// <summary>The movie or show chosen online (a TMDb or IMDb id). "" = the best search result.</summary>
    public string OnlineId { get; set; } = "";
    /// <summary>What the online lookup found for this disc, best first.</summary>
    public List<MediaMatch> LookupCandidates { get; set; } = new();
    public bool IsLookingUp { get; set; }
    /// <summary>Why the lookup found nothing, or null.</summary>
    public string? LookupMessage { get; set; }
    public int LookupGeneration { get; set; }
    /// <summary>Raised on the UI thread when a lookup starts or ends.</summary>
    public event Action? LookupChanged;
    public void RaiseLookupChanged() => LookupChanged?.Invoke();
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
        MediaYear = null;
        OnlineId = "";
        LookupCandidates = new();
        IsLookingUp = false;
        LookupMessage = null;
        LookupGeneration++;
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
    bool _betaKeyTried;
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
        RecoverUnfinishedJobs();
        CheckRecords = Engine.CheckRecords.Load();
        Background = new BackgroundQueue(_ui) { Limit = Config.BackgroundJobs };
        Background.Changed += UpdateKeepAwake;
        Web = new WebServer(this, _ui);
    }

    public BackgroundQueue Background { get; }
    public WebServer Web { get; }

    /// <summary>Starts the web page and, when enabled, the automatic beta key update. Call once at startup.</summary>
    public void StartServices()
    {
        Web.Apply(Config.WebUI);
        if (Config.AutoUpdateBetaKey) _ = UpdateBetaKeyIfNeededAsync("at startup");
    }

    /// <summary>With AutoUpdateBetaKey: registers the forum's current beta key when MakeMKV uses a beta key (or none) and
    /// it differs. A purchased key is never replaced.</summary>
    public async Task<string?> UpdateBetaKeyIfNeededAsync(string reason)
    {
        var installed = Config.RegistrationKey.Length > 0 ? Config.RegistrationKey : (MakeMKVEnvironment.InstalledSettings().TryGetValue("app_Key", out var ik) ? ik : null);
        if (!BetaKey.MayReplace(installed)) return null;
        string key;
        try { key = await BetaKey.FetchAsync(); } catch (Exception) { return null; }
        if (key == installed) return null;
        var result = await InstallBetaKeyAsync();
        LastError = $"Beta key updated {reason}: {result}";
        return result;
    }

    public async Task CloseTrayAsync(string lane)
    {
        if (EntryForLane(lane) is not { DevicePath.Length: > 0 } e) return;
        if (!await Platform.CloseTrayAsync(e.DevicePath)) LastError = $"Could not close the tray of {DriveItem.ShortModel(e.DriveName)}";
        await Task.Delay(5000);
        await RefreshDrivesAsync(true);
    }

    public async Task CloseAllTraysAsync()
    {
        foreach (var e in ScannedDrives.Where(d => d.IsPresent && d.State != DriveState.Inserted && d.DevicePath.Length > 0).ToList())
            await Platform.CloseTrayAsync(e.DevicePath);
        await Task.Delay(5000);
        await RefreshDrivesAsync(true);
    }

    static string WebState(JobState s) => s switch
    {
        JobState.Queued => "queued",
        JobState.Waiting => "waiting",
        JobState.Running => "running",
        _ => s.StatusWord(),
    };

    /// <summary>The JSON the web page shows (see shared/web/bromelia-web.html).</summary>
    public Dictionary<string, object> WebStatus() => new()
    {
        ["app"] = "Bromelia",
        ["version"] = System.Reflection.Assembly.GetEntryAssembly()?.GetName().Version?.ToString(3) ?? "",
        ["makemkv"] = MakemkvVersion,
        ["problem"] = MakemkvProblem?.Explanation ?? "",
        ["drives"] = DriveItems.Where(i => i.Entry != null).Select(i => new Dictionary<string, object>
        {
            ["lane"] = i.LaneKey, ["name"] = i.DisplayName, ["device"] = i.Entry!.DevicePath, ["state"] = i.Entry.State.DisplayName(),
            ["hasDisc"] = i.Entry.State == DriveState.Inserted, ["disc"] = i.Entry.DiscName, ["busy"] = ActiveJob(i.LaneKey) != null,
        }).ToList(),
        ["jobs"] = Jobs.Select(j => new Dictionary<string, object>
        {
            ["id"] = j.Id.ToString(), ["title"] = j.Title, ["state"] = WebState(j.State), ["stateLabel"] = j.State.Label(), ["phase"] = j.Phase,
            ["progress"] = j.OverallProgress, ["error"] = j.ErrorMessage ?? "", ["outputDirectory"] = j.OutputDirectory ?? "", ["lane"] = j.LaneKey,
        }).ToList(),
        ["background"] = Background.Items.Select(b => new Dictionary<string, object> { ["id"] = b.Id.ToString(), ["title"] = b.Work.Title, ["state"] = b.State }).ToList(),
        ["history"] = History.Take(20).Select(h => new Dictionary<string, object>
        {
            ["id"] = h.Id.ToString(), ["title"] = h.Title, ["state"] = WebState(h.State), ["stateLabel"] = h.State.Label(), ["error"] = h.ErrorMessage ?? "",
            ["outputDirectory"] = h.OutputDirectory ?? "", ["finishedAt"] = h.FinishedAt?.ToString("o") ?? "",
        }).ToList(),
        ["verify"] = new Dictionary<string, object>
        {
            ["running"] = Verify.Running, ["path"] = Verify.Path, ["progress"] = Verify.Total > 0 ? (double)Verify.Done / Verify.Total : 0.0,
            ["folder"] = Verify.Folder ?? "", ["stopped"] = Verify.Stopped, ["finishedAt"] = Verify.FinishedAt?.ToUniversalTime().ToString("o") ?? "",
            ["folders"] = Verify.Results.Count,
            ["damaged"] = Verify.Results.Where(r => !r.Ok).Select(r => new Dictionary<string, object> { ["folder"] = r.Folder, ["summary"] = r.Summary }).ToList(),
        },
    };

    /// <summary>Runs an action from the web page: drives/&lt;lane&gt;/rip|eject|close, jobs/&lt;id&gt;/cancel. Returns an error, or null.</summary>
    public string? WebAction(string kind, string id, string action)
    {
        switch (kind, action)
        {
            case ("drives", "rip" or "eject" or "close"):
                if (DriveItems.FirstOrDefault(d => d.LaneKey == id) is not { Entry: not null } item) return "No such drive";
                if (action == "rip")
                {
                    if (ActiveJob(id) != null) return "The drive is busy";
                    var before = Jobs.Count;
                    QuickRip(item);
                    return Jobs.Count > before ? null : LastError ?? "Nothing to rip";
                }
                _ = action == "eject" ? EjectAsync(id) : CloseTrayAsync(id);
                return null;
            case ("jobs", "cancel"):
                if (Jobs.FirstOrDefault(j => j.Id.ToString() == id) is not { } job) return "No such job";
                Cancel(job);
                return null;
            case ("verify", "cancel"):
                CancelVerify();
                return null;
            case ("verify", "start"):
                if (Verify.Running) return "A check is already running";
                if (!Directory.Exists(OutputRootPath)) return $"The output folder {OutputRootPath} doesn't exist";
                StartVerify(OutputRootPath);
                return null;
            default:
                return "Unknown action";
        }
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
        Background.Limit = Config.BackgroundJobs;
        Web.Apply(Config.WebUI);
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
        if (DiscContentRules.ModeFor(e.Flags, () => Platform.ProbeDisc(e.DevicePath), cfg) is not { } mode) return;
        var job = MakeJob(e, cfg, mode);
        job.UsesConfiguredMode = mode == cfg.Rip.Mode;
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
            LookUp(s);
        }
        catch (Exception e)
        {
            s.IsLoading = false;
            s.LoadError = e.Message;
        }
    }

    /// <summary>Looks the opened disc up online (with the name, kind and year chosen on the disc page) and keeps the candidates,
    /// so the disc page can show them.</summary>
    public void LookUp(DiscSession s)
    {
        var m = Config.Metadata;
        if (m.Provider == MetadataProvider.None || m.ApiKey.Trim().Length == 0 || s.Info is not { } info) return;
        var id = MediaIdentity.Resolve(info, info.Name, false, s.DiscFlags, null, s.MediaName, s.MediaKind);
        var (query, typedYear) = MetadataLookup.SplitYear(id.Name);
        var year = s.MediaYear ?? typedYear;
        var generation = ++s.LookupGeneration;
        s.IsLookingUp = true;
        s.LookupMessage = null;
        s.RaiseLookupChanged();
        _ = Task.Run(async () =>
        {
            List<MediaMatch> list = new();
            string? message = null;
            try
            {
                list = await MetadataLookup.SearchAsync(query, id.Kind, year, m).ConfigureAwait(false);
                if (list.Count == 0) message = $"{m.Provider.Label()} found nothing for “{query}”";
            }
            catch (Exception e) { message = e.Message; }
            _ui.Post(() =>
            {
                if (s.LookupGeneration != generation) return;
                s.LookupCandidates = list;
                s.LookupMessage = message;
                s.IsLookingUp = false;
                s.RaiseLookupChanged();
            });
        });
    }

    // --- jobs ---------------------------------------------------------------------------------

    public RipJob MakeJob(DriveScanEntry e, DriveConfig cfg, RipMode mode) =>
        new(new DiscSource.Drive(e.Index, e.DevicePath), cfg.Clone(), e.LaneKey, cfg.Name, e.DiscName, mode) { DiscFlags = e.Flags };

    static void ApplyIdentityChoices(DiscSession s, RipJob job)
    {
        job.MediaName = s.MediaName.Trim();
        job.MediaKind = s.MediaKind;
        job.FirstEpisode = s.FirstEpisode;
        job.MediaYear = s.MediaYear;
        job.OnlineId = s.OnlineId.Trim();
        job.DiscFlags ??= s.DiscFlags;
    }

    public void QuickRip(DriveItem item, RipMode? mode = null)
    {
        if (item.Entry == null) return;
        var cfg = item.Config ?? Config.DefaultDrive;
        var entry = item.Entry;
        var chosen = mode ?? DiscContentRules.ModeFor(entry.Flags, () => Platform.ProbeDisc(entry.DevicePath), cfg);
        if (chosen is not { } m)
        {
            LastError = $"{item.DisplayName}: this isn't a DVD or Blu-ray, and the drive is set to leave other discs alone";
            return;
        }
        var job = MakeJob(item.Entry, cfg, m);
        job.UsesConfiguredMode = mode == null && m == cfg.Rip.Mode;
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
        SaveUnfinishedJobs();
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
            MediaYear = job.MediaYear,
            OnlineId = job.OnlineId,
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
        SaveUnfinishedJobs(); // also picks up output folders chosen by running jobs
        StartScheduledCheckIfDue();
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
        job.ArchivedCandidates = ArchivedCandidates;
        var runner = new JobRunner(job, Config, exe, Mkvmerge, _ui, Platform, CatalogKeys);
        runner.Finished += finished =>
        {
            _runners.Remove(finished.Id);
            UpdateKeepAwake();
            if (finished.Background is { } work) Background.Enqueue(work);
            if (finished.MakemkvProblem is { IsLicenseProblem: true } problem)
            {
                MakemkvProblem = problem;
                if (problem.Kind == NoticeKind.KeyExpired && Config.AutoUpdateBetaKey && !_betaKeyTried)
                {
                    _betaKeyTried = true;
                    _ = UpdateBetaKeyIfNeededAsync("after it expired");
                }
            }
            RecordHistory(finished);
            _ = Task.Delay(3000).ContinueWith(_ => _ui.Post(() => _ = RefreshDrivesAsync(true)), TaskScheduler.Default);
            Pump();
        };
        _runners[job.Id] = runner;
        UpdateKeepAwake();
        _ = runner.RunAsync();
    }

    bool _keepingAwake;

    /// <summary>Whether the computer should be kept awake: jobs or background steps are running and PreventSleep is on.</summary>
    public bool WantsAwake => Config.PreventSleep && (_runners.Count > 0 || Background.IsBusy || Verify.Running);

    /// <summary>Keeps the computer awake while jobs or background steps run (when enabled in the settings).</summary>
    public void UpdateKeepAwake()
    {
        bool busy = WantsAwake;
        if (busy == _keepingAwake) return;
        _keepingAwake = busy;
        Platform.KeepAwake(busy);
    }

    void RecordHistory(RipJob job)
    {
        AddHistory(new HistoryRecord
        {
            Id = job.Id, Title = job.Title, DriveName = job.Drive.Name, DiscName = job.DiscLabel, Mode = job.Mode, State = job.State,
            StartedAt = job.StartedAt, FinishedAt = job.FinishedAt, OutputDirectory = job.OutputDirectory, Files = job.ProducedFiles.ToList(),
            ErrorMessage = job.ErrorMessage, LogPath = job.LogFile, Warnings = job.WarningCount, Errors = job.ErrorCount,
            Fingerprint = job.Fingerprint,
        });
        SaveUnfinishedJobs();
    }

    // --- archives: already archived, verifying ---------------------------------------------------------------

    /// <summary>Verifying archives again (Verify Archive…, the history, the web page, ArchiveCheck.IntervalDays).</summary>
    public sealed class VerifyStatus
    {
        public bool Running { get; init; }
        /// <summary>Started by ArchiveCheck.IntervalDays.</summary>
        public bool Scheduled { get; init; }
        /// <summary>The last run was stopped.</summary>
        public bool Stopped { get; init; }
        public string Path { get; init; } = "";
        public long Done { get; init; }
        public long Total { get; init; }
        public string? Folder { get; init; }
        public string? File { get; init; }
        /// <summary>The folders the last run finished.</summary>
        public IReadOnlyList<ArchiveVerifier.FolderCheck> Results { get; init; } = Array.Empty<ArchiveVerifier.FolderCheck>();
        public DateTime? FinishedAt { get; init; }
    }

    VerifyStatus _verify = new();
    /// <summary>Replaced as a whole on every change (PropertyChanged "Verify").</summary>
    public VerifyStatus Verify { get => _verify; private set => Set(ref _verify, value); }
    /// <summary>When each folder was last verified (archive-checks.json). Changes raise PropertyChanged "CheckRecords".</summary>
    public Dictionary<string, CheckRecord> CheckRecords { get; private set; } = new();
    CancellationTokenSource? _verifyCts;
    DateTime _nextScheduleCheck = DateTime.MaxValue;

    public string OutputRootPath => System.IO.Path.GetFullPath(Paths.ExpandUser(Config.OutputRoot));

    /// <summary>The history's successful jobs with their disc fingerprints.</summary>
    public List<ArchivedCandidate> ArchivedCandidates =>
        History.Where(h => !string.IsNullOrEmpty(h.Fingerprint) && h.OutputDirectory != null)
            .Select(h => new ArchivedCandidate(h.Fingerprint!, h.OutputDirectory!, h.State, h.FinishedAt)).ToList();

    /// <summary>The earlier archive of the disc with this listing according to the history (quick; for the disc page).</summary>
    public ArchivedMatch? ArchivedMatchFor(DiscInfo? info) => ArchiveLookup.Find(DiscFingerprint.Of(info), ArchivedCandidates, null);

    /// <summary>Whether ArchiveCheck.IntervalDays asks for a check of the output root at <paramref name="now"/>.</summary>
    public bool VerifyDue(DateTime now)
    {
        var days = Config.ArchiveCheck.IntervalDays;
        if (days <= 0 || Verify.Running || !Directory.Exists(OutputRootPath)) return false;
        return !CheckRecords.TryGetValue(OutputRootPath, out var last) || (now - last.CheckedAt).TotalDays >= days;
    }

    /// <summary>Starts the scheduled check when it is due (called from Pump, once a second; looks once an hour).</summary>
    void StartScheduledCheckIfDue()
    {
        var now = DateTime.Now;
        if (_nextScheduleCheck == DateTime.MaxValue) { _nextScheduleCheck = now.AddSeconds(60); return; }
        if (now < _nextScheduleCheck) return;
        _nextScheduleCheck = now.AddHours(1);
        if (VerifyDue(now) && ActiveJobCount == 0) StartVerify(OutputRootPath, scheduled: true);
    }

    /// <summary>Verifies every SHA256SUMS folder under <paramref name="path"/> on a worker thread. False when a check is already running.</summary>
    public bool StartVerify(string path, bool scheduled = false)
    {
        if (Verify.Running) return false;
        var cts = new CancellationTokenSource();
        _verifyCts = cts;
        Verify = new VerifyStatus { Running = true, Scheduled = scheduled, Path = path };
        UpdateKeepAwake();
        var targets = scheduled ? Config.Notifications.ToList() : new List<NotificationTarget>();
        var gate = new object();
        (long Done, long Total, string? Folder, string? File) live = (0, 0, null, null);
        var poll = new Timer(_ => _ui.Post(() =>
        {
            if (!Verify.Running) return;
            lock (gate) Verify = new VerifyStatus { Running = true, Scheduled = scheduled, Path = path, Done = live.Done, Total = live.Total, Folder = live.Folder, File = live.File };
        }), null, 250, 250);
        _ = Task.Run(async () =>
        {
            var (results, stopped) = ArchiveVerifier.Verify(ArchiveVerifier.Folders(path), (done, total, folder, file) =>
            {
                lock (gate) live = (done, total, folder ?? live.Folder, file ?? live.File);
                return !cts.IsCancellationRequested;
            });
            poll.Dispose();
            if (scheduled && !stopped && targets.Count > 0)
            {
                var report = VerifyReport(results);
                await NotificationSender.SendAsync(targets, report.Title, report.Body, report.Damaged ? "failed" : "success", _ => { });
            }
            _ui.Post(() => FinishVerify(path, results, stopped, scheduled));
        });
        return true;
    }

    public void CancelVerify() => _verifyCts?.Cancel();

    /// <summary>"Archive check: all 12 folder(s) OK" and the damaged folders, for notifications.</summary>
    public static (string Title, string Body, bool Damaged) VerifyReport(IReadOnlyList<ArchiveVerifier.FolderCheck> results)
    {
        var bad = results.Where(r => !r.Ok).ToList();
        var title = bad.Count == 0 ? $"Archive check: all {results.Count} folder(s) OK" : $"Archive check: {bad.Count} of {results.Count} folder(s) damaged";
        var body = string.Concat(bad.Take(10).Select(r => $"{r.Folder}: {r.Summary}\n"));
        if (bad.Count > 10) body += $"… and {bad.Count - 10} more\n";
        if (bad.Count == 0) body = "Every file matches its checksum.";
        return (title, body, bad.Count > 0);
    }

    void FinishVerify(string path, List<ArchiveVerifier.FolderCheck> results, bool stopped, bool scheduled)
    {
        var now = DateTime.Now;
        var records = new Dictionary<string, CheckRecord>(CheckRecords);
        foreach (var r in results) records[r.Folder] = new CheckRecord(now, r.Ok, r.Summary);
        // The whole tree, so a scheduled check knows when it last ran.
        if (!stopped && !results.Any(r => r.Folder == path))
        {
            var bad = results.Count(r => !r.Ok);
            records[path] = new CheckRecord(now, bad == 0, bad > 0 ? $"{bad} of {results.Count} archive folder(s) damaged" : $"{results.Count} archive folder(s), all OK");
        }
        CheckRecords = records;
        Engine.CheckRecords.Save(records);
        OnPropertyChanged(nameof(CheckRecords));
        Verify = new VerifyStatus { Path = path, Stopped = stopped, Results = results, FinishedAt = now, Scheduled = scheduled };
        _verifyCts = null;
        var report = VerifyReport(results);
        if (scheduled && report.Damaged)
        {
            LastError = report.Title;
            Platform.Notify(report.Title, report.Body, true);
        }
        UpdateKeepAwake();
    }

    string? _savedUnfinished;

    /// <summary>Writes the queued, waiting and running jobs to unfinished-&lt;pid&gt;.json (only when they changed).</summary>
    public void SaveUnfinishedJobs()
    {
        var entries = Jobs.Where(j => !j.State.IsFinished()).Select(j => new UnfinishedJobs.Entry
        {
            Id = j.Id, Title = j.Title, DriveName = j.Drive.Name, DiscName = j.DiscLabel, Mode = j.Mode, State = j.State,
            OutputDirectory = j.OutputDirectory, LogPath = j.LogFile, StartedAt = j.StartedAt,
        }).ToList();
        var json = entries.Count == 0 ? null : ConfigJson.Serialize(entries);
        if (json == _savedUnfinished) return;
        _savedUnfinished = json;
        UnfinishedJobs.Save(entries);
    }

    /// <summary>Records the jobs a Bromelia that has gone left unfinished (see <see cref="UnfinishedJobs"/>) and says so.</summary>
    public int RecoverUnfinishedJobs()
    {
        var records = UnfinishedJobs.Recover();
        if (records.Count == 0) return 0;
        foreach (var r in records) AddHistory(r);
        LastError = $"Bromelia stopped last time with {records.Count} unfinished job(s); they are in the history, marked as interrupted or not started";
        return records.Count;
    }

    void AddHistory(HistoryRecord rec)
    {
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
