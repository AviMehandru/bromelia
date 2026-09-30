using System.Globalization;
using System.Text;
using System.Text.Json;
using Bromelia.Core.Config;
using Bromelia.Core.Logic;
using Bromelia.Core.Robot;

namespace Bromelia.Core.Engine;

public sealed class JobException : Exception
{
    public JobException(string message) : base(message) { }
}

/// <summary>Platform services the engine needs from the host application.</summary>
public interface IPlatformServices
{
    /// <summary>What kind of disc is in the drive, when MakeMKV reports no DVD / Blu-ray structure.</summary>
    DiscContent ProbeDisc(string devicePath) => DiscContent.Unknown;
    Task<bool> CloseTrayAsync(string devicePath) => Task.FromResult(false);
    /// <summary>Whether the system has mounted (can read) the disc in the drive.</summary>
    bool IsDiscMounted(string devicePath) => true;
    Task<bool> EjectAsync(string devicePath);
    void Notify(string title, string body, bool sound);
    /// <summary>Keeps the computer from sleeping while <paramref name="on"/> (called on the UI thread).</summary>
    void KeepAwake(bool on) { }
}

public sealed class NullPlatformServices : IPlatformServices
{
    public Task<bool> EjectAsync(string devicePath) => Task.FromResult(false);
    public void Notify(string title, string body, bool sound) { }
}

/// <summary>Executes a single RipJob. Created and awaited on the UI thread.</summary>
public sealed class JobRunner
{
    sealed class RunSummary
    {
        public int ExitCode;
        public int? Saved;
        public int? Failed;
        public readonly List<string> ErrorMessages = new();
        public readonly DiscInfoBuilder Info = new();
        public string? DriveMismatch;
        /// <summary>First error of the run other than the "N titles saved, M failed" summary: usually the cause.</summary>
        public string? FirstError;
        /// <summary>MakeMKV's warning that the output may not fit on the destination (message 5038).</summary>
        public string? SpaceWarning;

        /// <summary>Why the run failed, for the job's error message.</summary>
        public string Reason => FirstError ?? ErrorMessages.LastOrDefault() ?? $"makemkvcon exit status {ExitCode}";
    }

    readonly RipJob _job;
    readonly AppConfig _config;
    readonly string _makemkvcon;
    readonly string? _mkvmerge;
    readonly UiDispatcher _ui;
    readonly IPlatformServices _platform;
    readonly IReadOnlyCollection<string> _catalogKeys;
    readonly CancellationTokenSource _cts = new();

    MakeMKVEnvironment _env = null!;
    MakeMKVEnvironment _baseEnv = null!;
    MakeMKVEnvironment? _allTracksEnv;
    ProcessRunner? _active;
    RunSummary _summary = new();
    (int Index, string Device)? _expectedDrive;
    string _lastTotalTitle = "";
    // Episodes (see PrepareEpisodesAsync)
    IVideoTsReader? _videoTs;
    DvdNavigation.EpisodePlan? _episodePlan;
    Dictionary<int, int> _separateEpisodes = new();   // MakeMKV title → episode offset
    int _firstEpisode = 1, _episodeWidth = 2;
    /// <summary>Episode number → title and plot, from the online lookup.</summary>
    Dictionary<int, EpisodeDetails> _episodeDetails = new();
    /// <summary>The kind the online lookup searched for.</summary>
    MediaKind? _lookedUpKind;
    /// <summary>The online match is the id chosen for this disc (not a search result).</summary>
    bool _metadataChosen;
    readonly Dictionary<string, int> _fileTitles = new(StringComparer.Ordinal);
    // Output (see FinishOutputAsync)
    string? _outputDir, _workDir;
    /// <summary>The output folder was created for this job (not a shared folder), so it may be renamed or removed.</summary>
    bool _ownsOutputDir;
    /// <summary>Error messages of the running makemkvcon count as read errors (rips and backups, not listings).</summary>
    bool _collectDataErrors;
    bool _reportedMissingVerifier;
    /// <summary>The main feature of a movie (the longest title ripped), for media server names.</summary>
    int? _mainTitle;

    /// <summary>Name prefix of the hidden staging folder a job writes to inside its output folder.</summary>
    public const string StagingPrefix = ".bromelia-incomplete-";

    public event Action<RipJob>? Finished;
    public bool CancelRequested { get; private set; }

    public JobRunner(RipJob job, AppConfig config, string makemkvcon, string? mkvmerge, UiDispatcher ui,
        IPlatformServices platform, IReadOnlyCollection<string> catalogKeys)
    {
        _job = job;
        _config = config;
        _makemkvcon = makemkvcon;
        _mkvmerge = mkvmerge;
        _ui = ui;
        _platform = platform;
        _catalogKeys = catalogKeys;
    }

    public void Cancel()
    {
        CancelRequested = true;
        _job.Phase = "Cancelling…";
        _cts.Cancel();
        _active?.Cancel();
    }

    public async Task RunAsync()
    {
        _job.State = JobState.Running;
        _job.StartedAt = DateTime.Now;
        _job.Phase = "Starting";
        _job.OpenLogFile();
        _job.AppendLog($"Bromelia job {_job.Id}");
        _job.AppendLog($"{_job.Mode.Label()} from {_job.Source.DisplayName} using configuration “{_job.Drive.Name}”");

        var status = JobState.Succeeded;
        try
        {
            await ExecuteAsync();
        }
        catch (Exception) when (_alreadyArchived != null)
        {
            status = JobState.Cancelled;
            _job.ErrorMessage = _alreadyArchived;
        }
        catch (Exception e) when (CancelRequested || e is OperationCanceledException)
        {
            status = JobState.Cancelled;
            _job.ErrorMessage = "Cancelled by user";
            _job.AppendLog("Job cancelled", Severity.Warning);
        }
        catch (Exception e)
        {
            status = JobState.Failed;
            _job.ErrorMessage = e.Message;
            _job.AppendLog(e.Message, Severity.Error);
        }
        if (CancelRequested && status == JobState.Succeeded) status = JobState.Cancelled;
        // Name the real cause when MakeMKV itself couldn't work (expired key, LibreDrive required, …).
        if (status == JobState.Failed && _job.MakemkvProblem is { } problem)
        {
            _job.ErrorMessage = $"{problem.Explanation} ({_job.ErrorMessage ?? "MakeMKV failed"})";
            _job.AppendLog(problem.Explanation, Severity.Error);
        }
        if (status == JobState.Succeeded && _job.DataErrors.Count > 0)
        {
            status = JobState.CompletedWithErrors;
            _job.ErrorMessage = $"MakeMKV reported {_job.DataErrors.Count} read error(s) while reading the disc, so the files may be damaged. They were kept apart from finished archives.";
            _job.AppendLog(_job.ErrorMessage, Severity.Error);
        }
        status = await FinishOutputAsync(status);
        if (status == JobState.Succeeded) await WriteMediaServerMetadataAsync();

        WriteManifest(status);
        // Background steps run later, in the background queue, once the disc is out.
        var allSteps = ApplicableSteps(status);
        var steps = allSteps.Where(s => !s.Background).ToList();
        var backgroundSteps = allSteps.Where(s => s.Background).ToList();
        if (steps.Count > 0 && !CancelRequested && _alreadyArchived == null)
        {
            _job.Phase = "Post-processing";
            _job.CurrentOperation = "";
            var ctx = new PostProcessor.Context(status, TemplateValues(status), _job.OutputDirectory, _job.ProducedFiles.ToList(),
                ScriptEnvironment(status));
            var results = await PostProcessor.RunAsync(steps, ctx, r => _active = r,
                (text, sev) => _ui.Post(() => _job.AppendLog(text, sev)), _cts.Token);
            await _ui.Barrier();
            foreach (var r in results.Where(r => r.ExitCode != 0 || r.TimedOut))
            {
                var step = steps.FirstOrDefault(s => s.Id == r.StepId);
                if (step is { FailJobOnError: true } && status == JobState.Succeeded)
                {
                    status = JobState.Failed;
                    _job.ErrorMessage = $"Post-processing step “{r.Name}” failed";
                }
            }
            if (CancelRequested && status == JobState.Succeeded) status = JobState.Cancelled;
        }

        if (_job.Source is DiscSource.Drive d &&
            (((status == JobState.Succeeded || (_alreadyArchived != null && _job.Drive.Automation.AlreadyArchived == AlreadyArchived.Skip))
              && _job.Drive.Automation.EjectWhenDone)
             || (status is JobState.Failed or JobState.CompletedWithErrors && _job.Drive.Automation.EjectOnFailure)))
        {
            _job.Phase = "Ejecting";
            var ok = await _platform.EjectAsync(d.DevicePath);
            _job.AppendLog(ok ? "Disc ejected" : $"Could not eject {d.DevicePath}", ok ? Severity.Info : Severity.Warning);
        }

        _job.State = status;
        _job.FinishedAt = DateTime.Now;
        _job.Phase = status.Label();
        _job.CurrentOperation = "";
        if (status == JobState.Succeeded) { _job.TotalProgress = 1; _job.CurrentProgress = 1; }
        WriteManifest(status);
        if (backgroundSteps.Count > 0 && !CancelRequested && _alreadyArchived == null)
        {
            _job.Background = new BackgroundWork(_job.Id, _job.Title, backgroundSteps,
                new PostProcessor.Context(status, TemplateValues(status), _job.OutputDirectory, _job.ProducedFiles.ToList(), ScriptEnvironment(status)),
                Path.Combine(_job.LogDirectory, "background.txt"));
            _job.AppendLog($"{backgroundSteps.Count} post-processing step(s) queued to run in the background");
        }
        var (title, body) = NotificationText(status);
        if (_config.Notifications.Count > 0)
        {
            await NotificationSender.SendAsync(_config.Notifications, title, body, status.StatusWord(), t => _ui.Post(() => _job.AppendLog(t, Severity.Warning)));
            await _ui.Barrier();
        }
        _job.AppendLog($"Finished: {status.Label()} in {FormatElapsed(_job.Elapsed)}");
        _job.CloseLogFile();
        _videoTs?.Dispose();
        if (_job.Drive.Archive.ArchiveRecord && _job.OutputDirectory is { } archiveDir && _job.ProducedFiles.Count > 0)
        {
            try { File.Copy(_job.LogFile, Paths.UniquePath(Path.Combine(archiveDir, "bromelia-log.txt"))); } catch (IOException) { } catch (UnauthorizedAccessException) { }
        }

        if (_job.Drive.Automation.Notify) _platform.Notify(title, body, _job.Drive.Automation.PlaySound);
        Finished?.Invoke(_job);
    }

    /// <summary>Title and text of the notification for a finished job.</summary>
    (string Title, string Body) NotificationText(JobState status)
    {
        var body = status switch
        {
            JobState.Succeeded => $"{_job.ProducedFiles.Count} item(s) saved to {_job.OutputDirectory}",
            JobState.CompletedWithErrors => $"Read errors: the files were kept in {_job.OutputDirectory}",
            JobState.Cancelled => _job.ErrorMessage ?? "Cancelled",
            _ => _job.ErrorMessage ?? "Failed",
        };
        var what = _job.Identity?.Name ?? (_job.DiscLabel.Length == 0 ? _job.SourceLabel : _job.DiscLabel);
        return ($"{_job.Mode.ShortLabel()} {(status == JobState.Succeeded ? "finished" : status.Label().ToLowerInvariant())}: {what}", body);
    }

    public static string FormatElapsed(TimeSpan t) =>
        t.TotalHours >= 1 ? t.ToString(@"h\:mm\:ss", CultureInfo.InvariantCulture) : t.ToString(@"m\:ss", CultureInfo.InvariantCulture);

    // --- pipeline ----------------------------------------------------------------------------

    async Task ExecuteAsync()
    {
        if (_job.Source is DiscSource.Drive { DevicePath.Length: > 0 } md && _job.IsAutomatic && _job.Drive.Automation.WaitForMountSeconds > 0)
        {
            _job.Phase = "Waiting for the disc to be mounted";
            var deadline = DateTime.Now.AddSeconds(_job.Drive.Automation.WaitForMountSeconds);
            bool mounted;
            while (!(mounted = _platform.IsDiscMounted(md.DevicePath)) && DateTime.Now < deadline && !CancelRequested)
                await Task.Delay(2000);
            if (CancelRequested) throw new OperationCanceledException();
            _job.AppendLog(mounted ? "The disc is mounted" : $"The disc wasn't mounted after {_job.Drive.Automation.WaitForMountSeconds} s; continuing");
        }
        if (_job.Mode is RipMode.AudioCD or RipMode.DataImage)
        {
            await ExecuteOtherDiscAsync();
            return;
        }
        bool needsAllTracks = _job.Mode.MakesMkv() && _job.TrackSelections.Count > 0;
        if (needsAllTracks && _mkvmerge == null)
            throw new JobException("Choosing individual tracks requires mkvmerge (MKVToolNix). Install MKVToolNix or reset the track choices.");
        var home = Path.Combine(_job.LogDirectory, "home");
        _baseEnv = _env = MakeMKVEnvironment.Prepare(_makemkvcon, _config, _job.Drive, home, _catalogKeys);
        if (needsAllTracks)
            _allTracksEnv = MakeMKVEnvironment.Prepare(_makemkvcon, _config, _job.Drive, Path.Combine(_job.LogDirectory, "home-alltracks"),
                _catalogKeys, "+sel:all");
        _job.AppendLog(MakeMKVEnvironment.UsesRegistry ? "MakeMKV settings are applied through the registry for each launch" : $"MakeMKV home: {home}");
        if (_env.ProfilePath != null) _job.AppendLog($"Profile: {_env.ProfilePath}");

        // The listing is always read again: titles chosen on an opened disc are only ripped when the disc
        // in the drive is still that disc and the titles can be found in the new listing.
        var opened = _job.PreloadedInfo;
        if (opened != null) _job.AppendLog("Reading the disc listing again to check that the disc hasn't changed since it was opened");
        DiscInfo? info = null;
        bool usingOpened = false;
        if (_job.Mode.MakesMkv() || _job.Mode == RipMode.InfoOnly) info = await ScanDiscAsync(_job.Source);
        else
        {
            // Backups don't need the listing, but it tells DVD, Blu-ray and 4K UHD apart and gives the title.
            try { info = await ScanDiscAsync(_job.Source); }
            catch (JobException e)
            {
                if (opened != null)
                {
                    info = opened;
                    usingOpened = true;
                    _job.AppendLog($"Using the listing read when the disc was opened: {e.Message}", Severity.Warning);
                }
                else _job.AppendLog($"Continuing without the disc listing: {e.Message}", Severity.Warning);
            }
        }
        if (opened != null && info != null && !usingOpened)
        {
            if (ListingMatcher.DifferentDisc(opened, info) is { } why)
                throw new JobException($"This is not the disc that was opened: {why}. Open the disc again.");
            ApplyTitleMap(opened, info);
        }
        _job.DiscInfo = info;
        if (info is { Name.Length: > 0 }) _job.DiscLabel = info.Name;
        ResolveIdentity(info);
        // Separate modes for DVDs, Blu-rays and 4K UHD discs (automatic and quick rips).
        if (_job.UsesConfiguredMode && _job.Drive.Rip.ModeFor(_job.Identity?.Format) is var m && m != _job.Mode)
        {
            _job.AppendLog($"{_job.Identity?.Format.Label() ?? "This disc"}: {m.Label()}");
            _job.Mode = m;
            ResolveIdentity(info);
        }
        _job.Fingerprint = DiscFingerprint.Of(info);
        if (_job.Fingerprint != null && _job.Mode != RipMode.InfoOnly) await CheckAlreadyArchivedAsync();
        await LookUpMetadataAsync();

        var outDir = PrepareOutput();

        switch (_job.Mode)
        {
            case RipMode.InfoOnly:
                _job.ProducedFiles.Add(WriteDiscInfo(info ?? throw new JobException("No disc information available"), outDir));
                break;
            case RipMode.Mkv:
                if (info == null) throw new JobException("No disc information available");
                if (_job.Drive.Rip.WriteDiscInfoJson) TryWriteDiscInfo(info, outDir);
                await RipTitlesAsync(_job.Source, info, outDir, 0);
                break;
            case RipMode.Backup:
            case RipMode.BackupDecrypted:
                _job.StepCount = 1;
                _job.ProducedFiles.Add(await BackupAsync(_job.Mode == RipMode.BackupDecrypted, outDir, false));
                break;
            case RipMode.AudioCD or RipMode.DataImage:
                break; // handled by ExecuteOtherDiscAsync
            case RipMode.BackupThenMkv:
                _job.StepCount = 2;
                var dest = await BackupAsync(true, outDir, true);
                _job.StepIndex = 1;
                DiscSource backupSource = Directory.Exists(dest) ? new DiscSource.Folder(dest) : new DiscSource.Iso(dest);
                var backupInfo = await ScanDiscAsync(backupSource);
                if (info != null) ApplyTitleMap(info, backupInfo);
                if (_job.Drive.Rip.WriteDiscInfoJson) TryWriteDiscInfo(backupInfo, outDir);
                await RipTitlesAsync(backupSource, backupInfo, outDir, 1);
                if (_job.Drive.Rip.KeepBackupAfterMkv) _job.ProducedFiles.Add(dest);
                else
                {
                    _job.AppendLog($"Removing backup {dest}");
                    try { if (Directory.Exists(dest)) Directory.Delete(dest, true); else File.Delete(dest); } catch (IOException) { }
                }
                break;
        }
    }

    /// <summary>Reserves the output folder and creates the hidden staging folder inside it, where everything is written
    /// until the job has finished and passed its checks.</summary>
    /// <summary>Why an automatic rip stopped: the disc was archived before (see CheckAlreadyArchivedAsync).</summary>
    string? _alreadyArchived;

    /// <summary>A disc archived before (same fingerprint, in the history or a bromelia.json under the output root): automatic rips
    /// follow Automation.AlreadyArchived, manual ones only warn. Throws (with _alreadyArchived set) to stop the job.</summary>
    async Task CheckAlreadyArchivedAsync()
    {
        _job.Phase = "Looking for earlier archives of this disc";
        var root = Paths.ExpandUser(_config.OutputRootFor(_job.Drive));
        var fingerprint = _job.Fingerprint;
        var candidates = _job.ArchivedCandidates.ToList();
        var m = await Task.Run(() => ArchiveLookup.Find(fingerprint, candidates, root));
        if (m == null) return;
        var when = m.ArchivedAt is { } t ? $" on {t.ToLocalTime():yyyy-MM-dd}" : "";
        var policy = _job.Drive.Automation.AlreadyArchived;
        if (!_job.IsAutomatic || policy == AlreadyArchived.RipAgain)
        {
            _job.AppendLog($"This disc was archived before, in {m.Folder}{when}; ripping it again", Severity.Warning);
            return;
        }
        _alreadyArchived = policy == AlreadyArchived.Skip
            ? $"Already archived in {m.Folder}{when}, so it wasn't ripped again. To archive it again, rip it from the drive page."
            : $"Already archived in {m.Folder}{when}. Rip it again from the drive page, or eject it.";
        _job.AppendLog(_alreadyArchived, Severity.Warning);
        throw new JobException(_alreadyArchived);
    }

    string PrepareOutput()
    {
        var finalDir = ResolveOutputDirectory();
        _outputDir = finalDir;
        _job.OutputDirectory = finalDir;
        var outDir = Path.Combine(finalDir, StagingPrefix + JobShortId);
        _workDir = outDir;
        var stage = Directory.CreateDirectory(outDir);
        try { stage.Attributes |= FileAttributes.Hidden; } catch (IOException) { }
        _job.AppendLog($"Output folder: {finalDir} (files are moved there once the job has finished and been checked)");
        return outDir;
    }

    /// <summary>The canonical title and year from TMDb or OMDb, when a provider is set up: the movie or show chosen for this disc
    /// (OnlineId), else the best search result for the name (with the year typed for the disc). Failures only log.</summary>
    async Task LookUpMetadataAsync()
    {
        var m = _config.Metadata;
        if (m.Provider == MetadataProvider.None || m.ApiKey.Trim().Length == 0 || _job.Identity is not { Name.Length: > 0 } id) return;
        _lookedUpKind = id.Kind;
        var chosen = _job.OnlineId.Trim();
        try
        {
            if (chosen.Length > 0)
            {
                if (OnlineId.Parse(chosen) is { } oid)
                {
                    _job.Phase = $"Looking up {oid.Label}";
                    if (await MetadataLookup.LookupAsync(oid, id.Kind, m) is { } match)
                    {
                        _metadataChosen = true;
                        Use(match, "chosen for this disc");
                    }
                    else _job.AppendLog($"{m.Provider.Label()} found nothing for {oid.Label}, chosen for this disc", Severity.Warning);
                    return;
                }
                _job.AppendLog($"“{chosen}” is not a TMDb or IMDb id; looking up the name instead", Severity.Warning);
            }
            var (query, typedYear) = MetadataLookup.SplitYear(id.Name);
            var year = _job.MediaYear ?? typedYear;
            _job.Phase = $"Looking up “{query}”";
            var list = await MetadataLookup.SearchAsync(query, id.Kind, year, m);
            if (list.Count == 0)
            {
                _job.AppendLog($"{m.Provider.Label()} found nothing for “{query}”{(year is { } y ? $" ({y})" : "")}", Severity.Warning);
                return;
            }
            var others = string.Join(", ", list.Skip(1).Take(4).Select(x => x.Label));
            Use(list[0], list.Count == 1 ? "the only result"
                : $"best of {list.Count} results; also {others}{(list.Count > 5 ? ", …" : "")}. Choose another on the disc page if this is wrong");
        }
        catch (Exception e) when (e is not OperationCanceledException)
        {
            _job.AppendLog($"Looking up “{id.Name}” failed: {e.Message}", Severity.Warning);
        }
    }

    void Use(MediaMatch match, string how)
    {
        _job.Metadata = match;
        var ids = string.Join(", ", new[] { match.TmdbId is { } t ? $"TMDb {t}" : null, match.ImdbId }.OfType<string>());
        _job.AppendLog($"{match.Provider}: “{match.Label}”{(ids.Length > 0 ? ", " + ids : "")} ({how})");
        ResolveIdentity(_job.DiscInfo);
    }

    /// <summary>Audio CDs (ripped with cyanrip / abcde) and data discs (copied sector by sector to an ISO image).</summary>
    async Task ExecuteOtherDiscAsync()
    {
        if (_job.Source is not DiscSource.Drive { DevicePath.Length: > 0 } drive) throw new JobException($"{_job.Mode.Label()} needs a disc in a drive");
        ResolveIdentity(null);
        var work = PrepareOutput();
        _job.StepCount = 1;
        if (_job.Mode == RipMode.DataImage)
        {
            var name = BackupName(true);
            var dest = Path.Combine(work, (name.Length > 0 ? name : "disc") + ".iso");
            _job.Phase = "Copying the disc to an ISO image";
            _job.AppendLog($"Copying {drive.DevicePath} to {Path.GetFileName(dest)}");
            try
            {
                await OtherDiscTools.CopyDiscAsync(drive.DevicePath, dest, n => _ui.Post(() => _job.CurrentOperation = TitleInfo.FormatBytes(n)), _cts.Token);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                throw new JobException($"Could not read the disc: {e.Message}");
            }
            await _ui.Barrier();
            _job.ProducedFiles.Add(dest);
            if (_job.Drive.Archive.VerifyRips && BackupVerifier.Problem(dest, true) is { } problem)
                throw new JobException($"The disc image failed the check: {problem}");
            return;
        }
        if (OtherDiscTools.AudioCommand(_job.Drive.Other, drive.DevicePath) is not { } cmd)
            throw new JobException("Ripping audio CDs needs cyanrip or abcde (both look up the album in MusicBrainz). Install one, or set an audio CD command.");
        _job.Phase = "Ripping the audio CD";
        var runner = new ProcessRunner(cmd.Exe, cmd.Args, null, work);
        _job.Commands.Add(runner.CommandLine);
        _job.AppendLog("$ " + runner.CommandLine);
        _active = runner;
        ProcessRunner.Result r;
        try { r = await runner.RunAsync(l => _ui.Post(() => _job.AppendLog(l)), default, _cts.Token, TimeSpan.FromMinutes(Math.Max(0, _config.StallTimeoutMinutes))); }
        catch (Exception e) when (e is System.ComponentModel.Win32Exception or FileNotFoundException)
        {
            throw new JobException($"Could not start {Path.GetFileName(cmd.Exe)}: {e.Message}");
        }
        finally { _active = null; }
        await _ui.Barrier();
        if (CancelRequested || r.Cancelled) throw new OperationCanceledException();
        if (r.Stalled) throw new JobException($"{Path.GetFileName(cmd.Exe)} printed nothing for {_config.StallTimeoutMinutes} minutes and was stopped");
        if (r.ExitCode != 0) throw new JobException($"{Path.GetFileName(cmd.Exe)} failed (exit status {r.ExitCode})");
        foreach (var n in VisibleItems(work)) _job.ProducedFiles.Add(Path.Combine(work, n));
        if (_job.ProducedFiles.Count == 0) throw new JobException($"{Path.GetFileName(cmd.Exe)} saved nothing");
    }

    /// <summary>Moves title and track choices made on one listing to the title numbers of another (the listing read
    /// when the job starts, or the listing of the backup). Throws when a chosen title can't be found.</summary>
    void ApplyTitleMap(DiscInfo old, DiscInfo now)
    {
        var chosen = (_job.ManualTitles ?? new List<int>()).Concat(_job.TrackSelections.Keys).Concat(_job.TitleNameOverrides.Keys).ToHashSet();
        if (chosen.Count == 0) return;
        var map = ListingMatcher.Map(chosen, old, now, _job.TrackSelections.Keys.ToHashSet());
        var moved = map.Where(kv => kv.Key != kv.Value).OrderBy(kv => kv.Key).ToList();
        if (moved.Count > 0) _job.AppendLog("Title numbers changed: " + string.Join(", ", moved.Select(kv => $"{kv.Key} → {kv.Value}")));
        if (_job.ManualTitles is { } manual) _job.ManualTitles = manual.Where(map.ContainsKey).Select(i => map[i]).OrderBy(i => i).ToList();
        _job.TrackSelections = _job.TrackSelections.Where(kv => map.ContainsKey(kv.Key)).ToDictionary(kv => map[kv.Key], kv => kv.Value);
        _job.TitleNameOverrides = _job.TitleNameOverrides.Where(kv => map.ContainsKey(kv.Key)).ToDictionary(kv => map[kv.Key], kv => kv.Value);
    }

    // --- identity ----------------------------------------------------------------------------------

    void ResolveIdentity(DiscInfo? info, int playAllEpisodes = 0, DiscFormat? format = null)
    {
        var id = MediaIdentity.Resolve(info, _job.DiscLabel, _job.Mode == RipMode.Backup, _job.DiscFlags, format ?? _job.Identity?.Format,
            _job.MediaName, _job.MediaKind, playAllEpisodes);
        if (_job.Metadata is { } meta)
        {
            id = id with { Name = TemplateRenderer.SanitizeComponent(meta.Title) };
            // An id chosen for the disc may be a show where a movie was assumed (or the other way round).
            if (_metadataChosen && meta.Kind is { } k && k != id.Kind && _job.MediaKind == null)
                id = id with { Kind = k, Reason = $"{meta.Provider} lists it as a {(k == MediaKind.Tv ? "TV show" : "movie")}" };
        }
        if (id != _job.Identity)
        {
            var set = id.Label.SetDescription;
            _job.AppendLog($"Identified as {(id.Kind == MediaKind.Tv ? "TV show" : "movie")} “{id.Name}”{(set.Length > 0 ? $" ({set})" : "")}, {id.Format.Label()}, format code {id.FormatCode} — {id.Reason}");
        }
        _job.Identity = id;
    }

    // --- episodes ----------------------------------------------------------------------------------

    static IVideoTsReader? OpenVideoTs(DiscSource source, string label) => source switch
    {
        DiscSource.Iso iso => IsoVideoTs.Open(iso.Path),
        DiscSource.Folder f => FolderVideoTs.Open(f.Path, label),
        DiscSource.Drive d when EpisodeSplitter.MountPoint(d.DevicePath) is { } mp => FolderVideoTs.Open(mp, label),
        _ => null,
    };

    /// <summary>Decides, before ripping, which titles are episodes: a DVD "play all" title that will be split, or
    /// separate episode-length titles. Also settles movie vs. TV and the first episode number.</summary>
    async Task PrepareEpisodesAsync(DiscSource source, DiscInfo info, List<int> indices)
    {
        _episodePlan = null;
        _separateEpisodes = new();
        var ripped = indices.Select(info.Title).OfType<TitleInfo>().ToList();
        var plans = new List<DvdNavigation.EpisodePlan>();
        if (_job.Identity?.Format == DiscFormat.Dvd && _job.Drive.Episodes.SplitPlayAll)
        {
            _job.Phase = "Reading the disc menus";
            _videoTs?.Dispose();
            _videoTs = await Task.Run(() => OpenVideoTs(source, _job.DiscLabel));
            var analysis = _videoTs == null ? null : await Task.Run(() => DvdNavigation.Analyse(_videoTs));
            if (analysis != null)
            {
                var sources = ripped.Select(t => t.SourceTitleId).OfType<int>().ToHashSet();
                plans = DvdNavigation.Plans(analysis).Where(p => sources.Contains(p.Title)).ToList();
                foreach (var p in plans)
                {
                    var eps = string.Join(", ", p.Starts.Select((c, i) => $"{c} ({DvdNavigation.Hms(p.ChapterStarts[c - 1])}, {p.Reasons[i]})"));
                    _job.AppendLog($"Disc title {p.Title}: menus start {p.EpisodeCount} episodes at chapters {eps}; last episode ends at chapter {p.LastEnd} ({p.EndRule})" +
                                   (p.Tail.Count > 0 ? $"; chapter(s) {string.Join(",", p.Tail)} follow it" : ""));
                }
            }
            else if (_videoTs == null) _job.AppendLog("Could not open the disc's VIDEO_TS to look for episodes", Severity.Warning);
        }
        var strict = plans.FirstOrDefault(p => p.IsPlausible(true))?.EpisodeCount ?? 0;
        ResolveIdentity(info, strict);
        // The menus showed a TV show where the lookup searched for a movie: search again, with the name read from the disc.
        if (_job.Identity is { } now && _lookedUpKind is { } searched && now.Kind != searched && !_metadataChosen)
        {
            _job.Metadata = null;
            ResolveIdentity(info, strict);
            await LookUpMetadataAsync();
        }
        if (_job.Identity?.Kind != MediaKind.Tv) return;

        if (plans.FirstOrDefault(p => p.IsPlausible(false)) is { } plan)
        {
            if (ripped.FirstOrDefault(t => t.SourceTitleId == plan.Title) is { } t && plan.MatchesChapterCount(t.ChapterCount)) _episodePlan = plan;
            else _job.AppendLog($"MakeMKV's title {plan.Title} has a different chapter count than the disc's navigation; not splitting it", Severity.Warning);
        }
        else if (plans.Count > 0)
            _job.AppendLog($"The menu jumps don't look like episodes (lengths {string.Join(", ", plans[0].EpisodeDurations.Select(DvdNavigation.Hms))}); not splitting");
        if (_episodePlan == null)
        {
            int k = 0;
            foreach (var t in MediaIdentity.EpisodeLikeTitles(ripped).OrderBy(t => t.Index)) _separateEpisodes[t.Index] = k++;
        }
        int count = _episodePlan?.EpisodeCount ?? _separateEpisodes.Count;
        if (count == 0) return;

        int? first = _job.FirstEpisode;
        var how = "entered for this disc";
        if (first == null && _job.Drive.Episodes.ReadMenuNumbers && _videoTs != null)
        {
            _job.Phase = "Reading episode numbers from the menus";
            var (numbers, why) = await EpisodeSplitter.OcrEpisodeNumbersAsync(_videoTs, _cts.Token);
            if (numbers != null && DvdNavigation.FirstEpisode(numbers, count) is { } f) { first = f; how = $"menu text [{string.Join(", ", numbers.OrderBy(n => n))}]"; }
            else how = why ?? $"menus read [{string.Join(", ", (numbers ?? new HashSet<int>()).OrderBy(n => n))}], no clear numbering";
        }
        if (await PreviousDiscAsync() is { } found)
        {
            if (first == null)
            {
                first = found.LastEpisode + 1;
                how = $"after episode {found.LastEpisode} of {found.Source}";
            }
            else if (first != found.LastEpisode + 1)
                _job.AppendLog($"The previous disc ended with episode {found.LastEpisode} ({found.Source}), but this disc starts with {first} ({how})", Severity.Warning);
        }
        if (first == null) how = $"numbered from 1 ({how})";
        _firstEpisode = first ?? 1;
        _episodeWidth = Math.Max(2, (_firstEpisode + count - 1).ToString(CultureInfo.InvariantCulture).Length);
        _job.AppendLog($"Episodes {_firstEpisode}–{_firstEpisode + count - 1} ({how})");
        await LookUpEpisodeTitlesAsync(count);
    }

    /// <summary>The last episode of the previous disc of this set (see <see cref="EpisodeContinuation"/>), for discs 2 and later.</summary>
    async Task<EpisodeContinuation.Found?> PreviousDiscAsync()
    {
        if (_job.Identity is not { Label.Disc: > 1 } id) return null;
        _job.Phase = "Looking for the previous disc's episodes";
        var q = new EpisodeContinuation.Query(id.Name, id.Label.Title, id.Label.Season, id.Label.Part, id.Label.Volume, id.Label.Disc!.Value);
        var root = Paths.ExpandUser(_config.OutputRootFor(_job.Drive));
        var folders = _job.ArchivedCandidates.Select(c => c.Folder).ToList();
        var season = id.Label.Season ?? 1;
        var seasonFolder = _job.Drive.Output.Layout == LibraryLayout.MediaServer && _outputDir != null
            ? Path.Combine(_outputDir, $"Season {season.ToString("00", CultureInfo.InvariantCulture)}") : null;
        return await Task.Run(() => EpisodeContinuation.Find(q, root, folders, seasonFolder, season));
    }

    /// <summary>The titles of this disc's episodes, when the show was found online (Metadata.EpisodeTitles). A season whose
    /// numbers don't cover every episode of the disc (numbered across seasons, say) gives no titles at all.</summary>
    async Task LookUpEpisodeTitlesAsync(int count)
    {
        _episodeDetails = new();
        var m = _config.Metadata;
        if (!m.EpisodeTitles || count == 0 || _job.Metadata is not { } match || _job.Identity?.Kind != MediaKind.Tv) return;
        var season = _job.Identity.Label.Season ?? 1;
        var wanted = Enumerable.Range(_firstEpisode, count).ToList();
        _job.Phase = "Looking up episode titles";
        try
        {
            var all = await MetadataLookup.EpisodesAsync(match, season, m);
            if (all.Count == 0)
            {
                _job.AppendLog($"{match.Provider} lists no episodes for season {season} of “{match.Title}”", Severity.Warning);
                return;
            }
            var missing = wanted.Where(n => !all.ContainsKey(n)).ToList();
            if (missing.Count > 0)
            {
                _job.AppendLog($"Season {season} of “{match.Title}” on {match.Provider} has episodes {all.Keys.Min()}–{all.Keys.Max()}, not {string.Join(", ", missing)}; the episodes get no titles", Severity.Warning);
                return;
            }
            foreach (var n in wanted) _episodeDetails[n] = all[n];
            _job.AppendLog($"Episode titles from {match.Provider}, season {season}: " + string.Join(", ", wanted.Select(n => $"{n} “{all[n].Title}”")));
        }
        catch (Exception e) when (e is not OperationCanceledException)
        {
            _job.AppendLog($"Looking up the episode titles failed: {e.Message}", Severity.Warning);
        }
    }

    /// <summary>Template values of episode <paramref name="n"/>.</summary>
    void SetEpisode(int n, Dictionary<string, string> values)
    {
        values["episode"] = MediaIdentity.EpisodeLabel(n, _episodeWidth);
        values["episodeNumber"] = n.ToString(CultureInfo.InvariantCulture);
        values["episodeTitle"] = _episodeDetails.TryGetValue(n, out var d) ? d.Title : "";
    }

    /// <summary>Splits the ripped "play all" title into episode files named with the file name template.</summary>
    async Task SplitEpisodesAsync(string outDir, DiscInfo info)
    {
        if (_episodePlan is not { } plan) return;
        var entry = _fileTitles.FirstOrDefault(kv => info.Title(kv.Value)?.SourceTitleId == plan.Title);
        if (entry.Key == null || info.Title(entry.Value) is not { } title) return;
        var file = entry.Key;
        var baseName = Path.GetFileName(file);
        if (_mkvmerge == null)
        {
            _job.AppendLog($"Splitting episodes needs mkvmerge (MKVToolNix); kept {baseName} as one file", Severity.Warning);
            return;
        }
        _job.Phase = "Splitting episodes";
        if (await EpisodeSplitter.DurationAsync(file, _mkvmerge) is { } d && Math.Abs(d - plan.Duration) > 2)
        {
            _job.AppendLog($"{baseName} lasts {DvdNavigation.Hms(d)}, the disc title {DvdNavigation.Hms(plan.Duration)}; not splitting", Severity.Warning);
            return;
        }
        var starts = await EpisodeSplitter.ChapterStartsAsync(file, EpisodeSplitter.FindTool("mkvextract", _mkvmerge));
        var split = DvdNavigation.MkvChapters(plan.SplitChapters, plan, starts);
        if (split == null)
        {
            _job.AppendLog($"The chapters of {baseName} don't line up with the disc's episodes; not splitting", Severity.Warning);
            return;
        }
        var parts = await EpisodeSplitter.SplitAsync(file, split, _mkvmerge, r => _active = r, l => _ui.Post(() => _job.AppendLog(l)));
        await _ui.Barrier();
        if (parts == null)
        {
            if (CancelRequested) throw new OperationCanceledException();
            _job.AppendLog($"mkvmerge could not split {baseName}; kept it as one file", Severity.Warning);
            return;
        }
        var outputs = new List<string>();
        for (int i = 0; i < parts.Count; i++)
        {
            var values = TemplateValues(JobState.Running);
            foreach (var kv in TitleValues(title, i + 1, info, parts[i])) values[kv.Key] = kv.Value;
            int firstCh, lastCh;
            if (i < plan.EpisodeCount)
            {
                (firstCh, lastCh) = plan.ChapterRange(i);
                SetEpisode(_firstEpisode + i, values);
            }
            else (firstCh, lastCh) = (plan.Tail[0], plan.Tail[^1]);
            values["track"] = $"{MediaIdentity.TrackLabel(title)} Ch {(firstCh == lastCh ? $"{firstCh}" : $"{firstCh}-{lastCh}")}";
            var fallback = $"{Path.GetFileNameWithoutExtension(file)} - {(i < plan.EpisodeCount ? $"Episode {_firstEpisode + i}" : "after last episode")}";
            var template = _job.Drive.Output.Layout == LibraryLayout.MediaServer ? MediaServerNaming.FileTemplate(values, false) : _job.Drive.Output.FileNameTemplate;
            var final = RenameTo(parts[i], values, template, fallback, outDir);
            outputs.Add(final);
            if (i < plan.EpisodeCount)
                _job.Episodes.Add(new ArchiveRecord.EpisodeEntry { File = RelativePath(final, outDir), Episode = _firstEpisode + i, SourceTitleId = plan.Title, FirstChapter = firstCh, LastChapter = lastCh,
                    Title = _episodeDetails.TryGetValue(_firstEpisode + i, out var ed) ? ed.Title : null });
        }
        _job.AppendLog($"Split {baseName} into {plan.EpisodeCount} episode(s){(parts.Count > plan.EpisodeCount ? " and a closing clip" : "")}");
        int at = _job.ProducedFiles.IndexOf(file);
        if (at < 0) at = _job.ProducedFiles.Count;
        if (!_job.Drive.Episodes.KeepPlayAll)
        {
            _job.ProducedFiles.Remove(file);
            try { File.Delete(file); } catch (IOException) { }
            _job.AppendLog($"Removed the unsplit title {baseName}");
        }
        else at++;
        foreach (var o in outputs) _job.ProducedFiles.Insert(Math.Min(at++, _job.ProducedFiles.Count), o);
    }

    static string RelativePath(string path, string baseDir)
    {
        var rel = Path.GetRelativePath(baseDir, path);
        return (rel.StartsWith("..", StringComparison.Ordinal) ? Path.GetFileName(path) : rel).Replace('\\', '/');
    }

    // --- archive -----------------------------------------------------------------------------------

    string JobShortId => _job.Id.ToString("N")[..8];

    /// <summary>Names of the entries of <paramref name="dir"/> that aren't hidden.</summary>
    public static List<string> VisibleItems(string dir) =>
        Directory.Exists(dir)
            ? Directory.EnumerateFileSystemEntries(dir).Select(e => Path.GetFileName(e)!).Where(n => !n.StartsWith('.')).OrderBy(n => n, StringComparer.Ordinal).ToList()
            : new List<string>();

    /// <summary>Moves the job's files out of the staging folder. A job that succeeded goes into the output folder.
    /// Anything else goes into a folder marked “[INCOMPLETE]” (failed or cancelled) or “[READ ERRORS]”, with a note
    /// explaining why, so an unfinished or damaged rip can never look like a finished archive. Checksums and the
    /// archive record are written for complete files, including files with read errors.</summary>
    /// <summary>Media server layout with an online match (Metadata.Nfo): the movie's or show's .nfo and poster in its folder (when
    /// missing: an earlier disc may have written them, or the server rewritten them), and an .nfo next to each episode. These
    /// are not in SHA256SUMS. Failures only log.</summary>
    async Task WriteMediaServerMetadataAsync()
    {
        if (!_config.Metadata.Nfo || _job.Drive.Output.Layout != LibraryLayout.MediaServer || _job.Metadata is not { } match
            || _job.OutputDirectory is not { } dir || _job.Identity is not { } id) return;
        _job.Phase = "Writing media server metadata";
        var written = new List<string>();
        void Write(string text, string path)
        {
            if (File.Exists(path)) return;
            try
            {
                using (var f = new FileStream(path, FileMode.CreateNew, FileAccess.Write)) f.Write(new UTF8Encoding(false).GetBytes(text));
                written.Add(RelativePath(path, dir));
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { _job.AppendLog($"Could not write {path}: {e.Message}", Severity.Warning); }
        }
        if (id.Kind == MediaKind.Tv)
        {
            Write(MediaServerMetadata.Nfo(match, MediaKind.Tv), Path.Combine(dir, "tvshow.nfo"));
            var season = id.Label.Season ?? 1;
            foreach (var e in _job.Episodes)
                if (e.Episode is { } n && _episodeDetails.TryGetValue(n, out var d))
                    Write(MediaServerMetadata.EpisodeNfo(match.Title, season, n, d),
                        Path.ChangeExtension(Path.Combine(dir, e.File.Replace('/', Path.DirectorySeparatorChar)), ".nfo"));
        }
        else if (_mainTitle is { } main && _fileTitles.FirstOrDefault(kv => kv.Value == main).Key is { } file)
            Write(MediaServerMetadata.Nfo(match, MediaKind.Movie), Path.ChangeExtension(file, ".nfo"));
        var poster = Path.Combine(dir, MediaServerMetadata.PosterName);
        if (match.Poster.Length > 0 && !File.Exists(poster) && Uri.TryCreate(match.Poster, UriKind.Absolute, out var url))
        {
            try
            {
                await MediaServerMetadata.DownloadAsync(url, poster);
                written.Add(MediaServerMetadata.PosterName);
            }
            catch (Exception e) when (e is HttpRequestException or TaskCanceledException or IOException or JobException)
            {
                _job.AppendLog($"Could not download the poster from {url.Host}: {e.Message}", Severity.Warning);
            }
        }
        if (written.Count > 0)
            _job.AppendLog("Wrote media server metadata: " + (written.Count > 4 ? string.Join(", ", written.Take(3)) + $" and {written.Count - 3} more" : string.Join(", ", written)));
    }

    async Task<JobState> FinishOutputAsync(JobState status)
    {
        if (_workDir is not { } stage || _outputDir is not { } outDir) return status;
        if (VisibleItems(stage).Count == 0)
        {
            TryDelete(stage);
            // Remove the folder reserved for this job when nothing was saved.
            if (status != JobState.Succeeded && _ownsOutputDir && Directory.Exists(outDir) &&
                Directory.EnumerateFileSystemEntries(outDir).All(e => Path.GetFileName(e) is ".DS_Store" or "desktop.ini" or "Thumbs.db"))
            {
                TryDelete(outDir);
                _job.OutputDirectory = null;
            }
            return status;
        }

        // Hash while the files are still in the staging folder: if a file can't be read back, it stays out of the archive.
        var entries = new List<Checksums.Entry>();
        var cfg = _job.Drive.Archive;
        if (status is JobState.Succeeded or JobState.CompletedWithErrors && (cfg.Checksums || cfg.ArchiveRecord) && _job.ProducedFiles.Count > 0)
        {
            try { entries = await HashProducedFilesAsync(stage); }
            catch (Exception e) when (CancelRequested || e is OperationCanceledException)
            {
                status = JobState.Cancelled;
                _job.ErrorMessage = "Cancelled by user";
                _job.AppendLog("Job cancelled", Severity.Warning);
            }
            catch (JobException e)
            {
                status = JobState.Failed;
                _job.ErrorMessage = e.Message;
                _job.AppendLog(e.Message, Severity.Error);
            }
        }

        string dest = outDir, from = stage;
        string? tag = status == JobState.Succeeded ? null : status == JobState.CompletedWithErrors ? "READ ERRORS" : "INCOMPLETE";
        if (tag != null) (dest, from) = MarkedFolder(outDir, stage, tag);

        _job.Phase = "Moving files";
        // Keys are paths relative to the staging folder ('/' separated); folders merged into existing ones have an entry per item.
        var moved = new Dictionary<string, string>(StringComparer.Ordinal);
        bool left = false;
        // A media server library keeps adding to the show's folders (Season 02, Other, Backup) instead of making new ones.
        bool merge = tag == null && _job.Drive.Output.Layout == LibraryLayout.MediaServer;
        void Move(string rel)
        {
            var source = Path.Combine(from, rel);
            var want = Path.Combine(dest, rel);
            if (merge && Directory.Exists(source) && Directory.Exists(want))
            {
                foreach (var name in VisibleItems(source)) Move(rel + "/" + name);
                return;
            }
            var target = Paths.UniquePath(want);
            try
            {
                if (Directory.Exists(source)) Directory.Move(source, target); else File.Move(source, target);
                moved[rel] = target;
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                left = true;
                _job.AppendLog($"Could not move {rel} out of {from}: {e.Message}", Severity.Error);
            }
        }
        foreach (var name in VisibleItems(from)) Move(name);
        if (left)
        {
            if (status == JobState.Succeeded)
            {
                status = JobState.Failed;
                _job.ErrorMessage = $"Some files could not be moved into the output folder; they are still in {from}";
            }
        }
        else TryDelete(from);   // only temporary (hidden) files are left

        // Paths recorded while the files were in the staging folder now point to their final place.
        var stagePath = Path.GetFullPath(stage).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar) + Path.DirectorySeparatorChar;
        string Relocate(string p)
        {
            var full = Path.GetFullPath(p);
            if (!full.StartsWith(stagePath, StringComparison.OrdinalIgnoreCase)) return p;
            var rest = full[stagePath.Length..].Replace(Path.DirectorySeparatorChar, '/');
            for (var key = rest; ;)
            {
                if (moved.TryGetValue(key, out var top))
                    return key.Length == rest.Length ? top : Path.Combine(top, rest[(key.Length + 1)..].Replace('/', Path.DirectorySeparatorChar));
                var slash = key.LastIndexOf('/');
                if (slash < 0) return p;
                key = key[..slash];
            }
        }
        string RelocateRelative(string r) => RelativePath(Relocate(Path.Combine(stage, r.Replace('/', Path.DirectorySeparatorChar))), dest);
        for (int i = 0; i < _job.ProducedFiles.Count; i++) _job.ProducedFiles[i] = Relocate(_job.ProducedFiles[i]);
        var titles = _fileTitles.ToList();
        _fileTitles.Clear();
        foreach (var kv in titles) _fileTitles[Relocate(kv.Key)] = kv.Value;
        foreach (var e in _job.Episodes) e.File = RelocateRelative(e.File);
        entries = entries.Select(e => e with { Path = RelocateRelative(e.Path) }).ToList();
        _job.OutputDirectory = dest;
        _job.AppendLog(tag == null ? $"Moved {moved.Count} item(s) into {dest}" : $"Kept {moved.Count} item(s) apart in {dest}",
            tag == null ? Severity.Info : Severity.Warning);

        if (status is JobState.Succeeded or JobState.CompletedWithErrors && WriteArchive(entries, dest, status) is { } error && status == JobState.Succeeded)
        {
            status = JobState.Failed;
            _job.ErrorMessage = error;
        }
        if (status != JobState.Succeeded) WriteNote(dest, tag ?? "INCOMPLETE", status);
        return status;
    }

    static void TryDelete(string path)
    {
        try { if (Directory.Exists(path)) Directory.Delete(path, true); else if (File.Exists(path)) File.Delete(path); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
    }

    /// <summary>Where the files of a job that didn't succeed go. When the output folder was created for this job it is
    /// renamed (“Name [INCOMPLETE]”); in a shared folder a subfolder is used (“INCOMPLETE - 1a2b3c4d”).
    /// Returns that folder and the staging folder's (possibly new) location.</summary>
    (string Dest, string Stage) MarkedFolder(string outDir, string stage, string tag)
    {
        // Kept outside a media server library, where the server would pick the files up.
        if (_job.Drive.Output.Layout == LibraryLayout.MediaServer)
        {
            var root = Paths.ExpandUser(_config.OutputRootFor(_job.Drive));
            if (root.Length == 0) root = Paths.DefaultOutputRoot;
            var d = Paths.UniquePath(Path.Combine(root, TemplateRenderer.SanitizeComponent($"{Path.GetFileName(outDir.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar))} [{tag} {JobShortId}]")));
            Directory.CreateDirectory(d);
            return (d, stage);
        }
        if (_ownsOutputDir && VisibleItems(outDir).Count == 0)
        {
            var parent = Path.GetDirectoryName(Path.GetFullPath(outDir).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar))!;
            var target = Paths.UniquePath(Path.Combine(parent, $"{Path.GetFileName(outDir.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar))} [{tag}]"));
            try
            {
                Directory.Move(outDir, target);
                _outputDir = target;
                return (target, Path.Combine(target, Path.GetFileName(stage)));
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                _job.AppendLog($"Could not rename {outDir}: {e.Message}", Severity.Warning);
            }
        }
        var sub = Paths.UniquePath(Path.Combine(outDir, $"{tag} - {JobShortId}"));
        Directory.CreateDirectory(sub);
        return (sub, stage);
    }

    /// <summary>Explains in the folder itself why its files are not a finished archive.</summary>
    void WriteNote(string dir, string tag, JobState status)
    {
        var text = new StringBuilder();
        text.Append($"Bromelia job {_job.Id}: {status.Label()}.\n");
        text.Append("These files are NOT a finished archive. Rip the disc again (clean it first if it has read errors),\n");
        text.Append("or check the files yourself before using them.\n\n");
        if (_job.ErrorMessage != null) text.Append(_job.ErrorMessage).Append("\n\n");
        if (_job.DataErrors.Count > 0)
        {
            text.Append("Errors reported by MakeMKV while reading the disc:\n");
            foreach (var e in _job.DataErrors) text.Append("  ").Append(e).Append('\n');
            text.Append('\n');
        }
        text.Append($"Full log: {_job.LogFile}\n");
        try { File.WriteAllText(Path.Combine(dir, $"{tag}.txt"), text.ToString()); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
    }

    /// <summary>SHA-256 of every produced file (folders are walked), paths relative to <paramref name="baseDir"/>.</summary>
    async Task<List<Checksums.Entry>> HashProducedFilesAsync(string baseDir)
    {
        var files = Checksums.Files(_job.ProducedFiles, baseDir);
        var sizes = files.Select(f => new FileInfo(f.Full).Length).ToList();
        long total = Math.Max(1, sizes.Sum()), done = 0;
        _job.Phase = "Computing checksums";
        _job.StepIndex = _job.StepCount;
        _job.TotalProgress = 0;
        var entries = new List<Checksums.Entry>();
        for (int i = 0; i < files.Count; i++)
        {
            if (CancelRequested) throw new OperationCanceledException();
            _job.CurrentOperation = files[i].Relative;
            long before = done, size = Math.Max(1, sizes[i]);
            long lastTick = 0;
            string hash;
            try
            {
                hash = await Task.Run(() => Checksums.Sha256(files[i].Full, n =>
                {
                    var now = Environment.TickCount64;
                    if (now - Interlocked.Read(ref lastTick) < 100) return;
                    Interlocked.Exchange(ref lastTick, now);
                    _ui.Post(() => { _job.CurrentProgress = (double)n / size; _job.TotalProgress = (double)(before + n) / total; });
                }, _cts.Token));
            }
            catch (OperationCanceledException) { throw; }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                throw new JobException($"Could not read {files[i].Relative} to compute its checksum: {e.Message}");
            }
            await _ui.Barrier();
            done += sizes[i];
            entries.Add(new Checksums.Entry(files[i].Relative, sizes[i], hash));
        }
        _job.CurrentOperation = "";
        return entries;
    }

    /// <summary>Writes SHA256SUMS and bromelia.json into <paramref name="dir"/>. Returns an error message when SHA256SUMS can't be written.</summary>
    string? WriteArchive(List<Checksums.Entry> entries, string dir, JobState status)
    {
        var cfg = _job.Drive.Archive;
        _job.Checksums = entries;
        if (cfg.Checksums && entries.Count > 0)
        {
            try
            {
                _job.ChecksumFile = Checksums.WriteMerged(dir, entries);
                _job.AppendLog($"Wrote SHA-256 checksums of {entries.Count} file(s) to {Checksums.FileName}");
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                var msg = $"Could not write {Path.Combine(dir, Checksums.FileName)}: {e.Message}";
                _job.AppendLog(msg, Severity.Error);
                return msg;
            }
        }
        if (cfg.ArchiveRecord) WriteArchiveRecord(dir, status);
        return null;
    }

    void WriteArchiveRecord(string outDir, JobState status)
    {
        if (_job.Identity is not { } id) return;
        var info = _job.RipInfo ?? _job.DiscInfo;
        var record = new ArchiveRecord
        {
            Status = status.StatusWord(),
            Name = id.Name,
            Kind = id.Kind.Token(),
            Disc = new ArchiveRecord.DiscEntry
            {
                Label = _job.DiscLabel, VolumeName = info?.VolumeName ?? "", Type = info?.TypeName ?? "", Format = id.Format.Token(),
                FormatCode = id.FormatCode, Encrypted = id.Encrypted, Season = id.Label.Season, Part = id.Label.Part, Volume = id.Label.Volume, Disc = id.Label.Disc,
                Fingerprint = _job.Fingerprint,
            },
            Rip = _job.Mode.MakesMkv() ? "Rip" : "Backup",
            Mode = JsonNamingPolicy.CamelCase.ConvertName(_job.Mode.ToString()),
            Source = _job.Source.InfoArgument,
            DriveName = _job.Drive.Name,
            Makemkv = _job.MakemkvVersion,
            JobId = _job.Id.ToString(),
            StartedAt = _job.StartedAt,
            FinishedAt = DateTime.Now,
            Titles = _job.RipTitles.Select(i => info?.Title(i)).OfType<TitleInfo>().Select(t => new ArchiveRecord.TitleEntry
            {
                Index = t.Index, SourceTitleId = t.SourceTitleId, SourceFile = t.SourceFileName, Duration = t.DurationText,
                Chapters = t.ChapterCount, SizeBytes = t.SizeBytes, SegmentMap = t.SegmentMap,
            }).ToList(),
            Episodes = _job.Episodes.ToList(),
            Files = _job.Checksums,
            Warnings = _job.WarningCount,
            Errors = _job.ErrorCount,
            ErrorMessages = _job.ErrorMessages.ToList(),
            ReadErrors = _job.DataErrors.ToList(),
            LibreDrive = _job.LibreDrive,
        };
        var path = Paths.UniquePath(Path.Combine(outDir, "bromelia.json"));
        try
        {
            File.WriteAllText(path, record.ToJson());
            _job.AppendLog($"Wrote the archive record {Path.GetFileName(path)}");
        }
        catch (IOException e) { _job.AppendLog($"Could not write {path}: {e.Message}", Severity.Warning); }
    }

    async Task<RunSummary> RunMakeMkvAsync(List<string> args, bool readsData = false)
    {
        if (CancelRequested) throw new OperationCanceledException();
        var summary = new RunSummary();
        _collectDataErrors = readsData;
        _summary = summary;
        _lastTotalTitle = "";
        bool showDebug = _env.Settings.TryGetValue("app_ShowDebug", out var dbg) && dbg == "1";
        ProcessRunner.Result result;
        try
        {
            result = await _env.RunAsync(args,
                line =>
                {
                    if (RobotParser.Parse(line) is { } ev) _ui.Post(() => Handle(ev, summary, showDebug));
                },
                r =>
                {
                    _active = r;
                    _job.Commands.Add(r.CommandLine);
                    _job.AppendLog("$ " + r.CommandLine);
                }, default, _cts.Token, TimeSpan.FromMinutes(Math.Max(0, _config.StallTimeoutMinutes)));
        }
        catch (Exception e) when (e is System.ComponentModel.Win32Exception or FileNotFoundException)
        {
            throw new JobException($"Could not start makemkvcon: {e.Message}");
        }
        finally
        {
            _active = null;
        }
        await _ui.Barrier();
        _collectDataErrors = false;
        summary.ExitCode = result.ExitCode;
        // These stop makemkvcon themselves, so they are checked before cancellation.
        if (!CancelRequested)
        {
            if (summary.DriveMismatch != null) throw new JobException(summary.DriveMismatch);
            if (summary.SpaceWarning != null)
                throw new JobException($"Not enough free space on the destination, so the rip was stopped before writing. MakeMKV: “{summary.SpaceWarning}”");
            if (result.Stalled)
                throw new JobException($"makemkvcon printed nothing for {_config.StallTimeoutMinutes} minutes and was stopped{(result.Abandoned ? " (it did not exit; the drive may need to be reset)" : "")}. The drive or disc may be stuck: eject the disc and retry.");
        }
        if (CancelRequested || result.Cancelled || result.Abandoned) throw new OperationCanceledException();
        return summary;
    }

    void Handle(RobotEvent ev, RunSummary s, bool showDebug)
    {
        s.Info.Consume(ev);
        switch (ev)
        {
            case RobotEvent.Message { Value: var m }:
                if (MakeMKVNotice.From(m) is { } notice)
                {
                    if (notice.Kind == NoticeKind.LibreDrive) _job.LibreDrive ??= notice.Detail;
                    else _job.MakemkvProblem ??= notice;
                }
                var sev = m.Severity;
                if (sev == Severity.Debug && !showDebug) return;
                _job.AppendLog(m.Text, sev);
                if (sev == Severity.Error)
                {
                    if (s.FirstError == null && m.Code is not (5037 or 5004)) s.FirstError = m.Text;
                    s.ErrorMessages.Add(m.Text);
                    _job.ErrorMessages.Add(m.Text);
                    if (_collectDataErrors) _job.DataErrors.Add(m.Text);
                }
                if (m.Code == 1005 && _job.MakemkvVersion.Length == 0) _job.MakemkvVersion = m.Parameters.Count > 0 ? m.Parameters[0] : m.Text;
                if (m.Code is 5036 or 5005 && m.Parameters.Count > 0 && int.TryParse(m.Parameters[0], out var saved)) s.Saved = saved;
                if (m.Code == 5038 && s.SpaceWarning == null)
                {
                    // "The total size of all output files may reach as much as … while there are only … free":
                    // stop before anything is written rather than fail when the disk fills up.
                    s.SpaceWarning = m.Text;
                    _active?.Cancel();
                }
                if (m.Code is 5037 or 5004 && m.Parameters.Count > 1)
                {
                    if (int.TryParse(m.Parameters[0], out var sv)) s.Saved = sv;
                    if (int.TryParse(m.Parameters[1], out var fl)) s.Failed = fl;
                }
                break;
            case RobotEvent.ProgressTotalTitle t:
                _job.TotalOperation = t.Name;
                if (t.Name != _lastTotalTitle) { _lastTotalTitle = t.Name; _job.AppendLog("— " + t.Name); }
                break;
            case RobotEvent.ProgressCurrentTitle c:
                _job.CurrentOperation = c.Name;
                break;
            case RobotEvent.ProgressValue v when v.Max > 0:
                _job.CurrentProgress = (double)v.Current / v.Max;
                _job.TotalProgress = (double)v.Total / v.Max;
                break;
            case RobotEvent.Drive { Entry: var d }:
                if (_expectedDrive is { } exp && d.Index == exp.Index && exp.Device.Length > 0 && d.DevicePath.Length > 0 &&
                    !string.Equals(d.DevicePath, exp.Device, StringComparison.OrdinalIgnoreCase))
                {
                    s.DriveMismatch = $"MakeMKV drive {exp.Index} is now {d.DevicePath}, expected {exp.Device}. The job was stopped to avoid reading the wrong disc; rescan drives and retry.";
                    _active?.Cancel();
                }
                break;
            case RobotEvent.Raw r:
                _job.AppendLog(r.Line);
                break;
        }
    }

    async Task<DiscInfo> ScanDiscAsync(DiscSource source)
    {
        _job.Phase = "Reading disc information";
        var s = await RunMakeMkvAsync(_env.InfoArguments(source, _job.Drive.Rip));
        var info = s.Info.Info;
        if (info.Titles.Count == 0)
        {
            var reason = s.ErrorMessages.LastOrDefault() ?? $"makemkvcon reported no titles (exit status {s.ExitCode})";
            throw new JobException($"Could not read titles from {source.DisplayName}: {reason}");
        }
        _job.AppendLog($"Found {info.Titles.Count} title(s) on “{info.Name}”");
        return info;
    }

    List<int> ChooseTitles(DiscInfo info)
    {
        if (_job.ManualTitles is { } manual)
        {
            var valid = manual.Where(i => info.Title(i) != null).OrderBy(i => i).ToList();
            if (valid.Count == 0) throw new JobException("None of the chosen titles exist on the disc");
            return valid;
        }
        var result = TitleSelector.Evaluate(info.Titles, _job.Drive.Rip.TitleSelection);
        if (result.Error != null) throw new JobException(result.Error);
        if (result.RequiresManualChoice)
            throw new JobException($"“{_job.Drive.Name}” is set to choose titles manually. Open the disc and pick titles, or change the title rules.");
        foreach (var d in result.Decisions)
        {
            var t = info.Title(d.TitleIndex);
            _job.AppendLog($"Title {d.TitleIndex} ({t?.DurationText ?? "?"}, {t?.ChapterCount ?? 0} ch): {(d.Selected ? "selected" : "skipped — " + d.Reason)}");
        }
        if (result.SelectedIndices.Count == 0) throw new JobException("No titles matched the title selection rules");
        return result.SelectedIndices;
    }

    async Task RipTitlesAsync(DiscSource source, DiscInfo discInfo, string outDir, int extraSteps)
    {
        var info = discInfo;
        var indices = ChooseTitles(info);
        var ripConfig = _job.Drive.Rip;
        // Rip several titles in one makemkvcon run (one read of the disc structure) when they are exactly the
        // titles above some length: then a minimum length leaves just them, and "all" rips them.
        if (await OnePassListingAsync(source, info, indices) is { } pass)
        {
            info = pass.Info;
            indices = info.Titles.Select(t => t.Index).ToList();
            ripConfig = ripConfig.Clone();
            ripConfig.MinLengthSeconds = pass.MinLength;
        }
        _job.RipInfo = info;
        _job.RipTitles = indices;
        _mainTitle = indices.Select(info.Title).OfType<TitleInfo>().OrderByDescending(t => t.DurationSeconds).FirstOrDefault()?.Index;
        _fileTitles.Clear();
        await PrepareEpisodesAsync(source, info, indices);
        CheckFreeSpace(indices.Select(info.Title).OfType<TitleInfo>().ToList(), outDir);
        bool everyTitle = indices.ToHashSet().SetEquals(info.Titles.Select(t => t.Index));
        bool single = everyTitle && _job.TrackSelections.Count == 0;
        var invocations = single ? new List<string> { "all" } : indices.Select(i => i.ToString(CultureInfo.InvariantCulture)).ToList();
        _job.StepCount = extraSteps + invocations.Count;
        var failures = new List<string>();

        for (int n = 0; n < invocations.Count; n++)
        {
            if (CancelRequested) throw new OperationCanceledException();
            var t = invocations[n];
            _job.StepIndex = extraSteps + n;
            _job.TotalProgress = 0;
            _job.CurrentProgress = 0;
            _job.Phase = single ? $"Ripping {indices.Count} title(s)" : $"Ripping title {t} ({n + 1} of {invocations.Count})";
            var before = MkvFiles(outDir);
            int.TryParse(t, out var tIndex);
            _env = !single && _job.TrackSelections.ContainsKey(tIndex) ? _allTracksEnv ?? _baseEnv : _baseEnv;
            RunSummary s;
            try { s = await RunMakeMkvAsync(_env.MkvArguments(source, t, outDir, ripConfig), readsData: true); }
            finally { _env = _baseEnv; }
            var produced = MkvFiles(outDir).Except(before).OrderBy(p => p, StringComparer.Ordinal).ToList();
            bool failed = s.ExitCode != 0 || (s.Failed ?? 0) > 0 || produced.Count == 0;
            if (failed)
            {
                var why = s.Reason;
                failures.Add(single ? why : $"title {t}: {why}");
                _job.AppendLog($"Title {t} failed: {why}", Severity.Error);
                if (single && produced.Count == 0) break;
            }
            foreach (var file in produced)
            {
                int? titleIndex = single ? info.Titles.FirstOrDefault(x => x.OutputFileName == Path.GetFileName(file))?.Index : tIndex;
                if (failed)
                {
                    // A file of a title that didn't finish keeps MakeMKV's name, so it can't pass for a finished one.
                    _job.AppendLog($"Kept {Path.GetFileName(file)} under MakeMKV's name: the title did not finish", Severity.Warning);
                    _job.ProducedFiles.Add(file);
                    continue;
                }
                if (titleIndex is { } vi && info.Title(vi) is { } checkedTitle && await VerifyAsync(file, checkedTitle) is { } problem)
                {
                    failures.Add($"title {vi}: {problem}");
                    _job.AppendLog($"Title {vi} failed the check: {problem}", Severity.Error);
                    _job.ProducedFiles.Add(file);
                    continue;
                }
                var final = file;
                if (titleIndex is { } ti && info.Title(ti) is { } title)
                {
                    if (_job.TrackSelections.TryGetValue(ti, out var keep)) final = await RemuxAsync(final, title, keep);
                    final = Rename(final, title, indices.IndexOf(ti) + 1, info, outDir);
                    _fileTitles[final] = ti;
                }
                _job.ProducedFiles.Add(final);
            }
        }
        _job.StepIndex = _job.StepCount;
        if (failures.Count > 0)
            throw new JobException(failures.Count == 1 ? $"Rip failed: {failures[0]}" : $"{failures.Count} titles failed: {string.Join("; ", failures)}");
        await SplitEpisodesAsync(outDir, info);
    }

    async Task<string> BackupAsync(bool decrypt, string outDir, bool inSubfolder)
    {
        if (_job.Source is not DiscSource.Drive drive) throw new JobException("Backups can only be made from a disc in a drive");
        var dest = outDir;
        if (_job.Drive.Output.Layout == LibraryLayout.MediaServer)
        {
            // Kept out of the media server's scan (.plexignore for Plex, .ignore for Jellyfin / Emby).
            dest = Path.Combine(outDir, "Backup");
            Directory.CreateDirectory(dest);
            try { File.WriteAllText(Path.Combine(dest, ".plexignore"), "*\n"); File.WriteAllText(Path.Combine(dest, ".ignore"), ""); } catch (IOException) { }
        }
        else if (inSubfolder)
        {
            var sub = TemplateRenderer.RenderPath(_job.Drive.Output.BackupSubfolder, TemplateValues(JobState.Running));
            dest = Path.Combine(outDir, sub.Length == 0 ? "backup" : sub);
        }
        Directory.CreateDirectory(dest);
        var name = BackupName(decrypt);
        if (_job.Drive.Rip.BackupFormat == BackupFormat.Iso)
            dest = Paths.UniquePath(Path.Combine(dest, (name.Length > 0 ? name : TemplateRenderer.SanitizeComponent(_job.DiscLabel.Length == 0 ? "disc" : _job.DiscLabel)) + ".iso"));
        else if (name.Length > 0)
        {
            dest = Paths.UniquePath(Path.Combine(dest, name));
            Directory.CreateDirectory(dest);
        }
        _job.Phase = decrypt ? "Backing up disc (decrypted)" : "Backing up disc";
        _job.TotalProgress = 0;
        _expectedDrive = (drive.Index, drive.DevicePath);
        try
        {
            var args = _env.BackupArguments(_job.Source, decrypt, dest, _job.Drive.Rip) ?? throw new JobException("Backups require a drive source");
            var s = await RunMakeMkvAsync(args, readsData: true);
            bool exists = File.Exists(dest) || (Directory.Exists(dest) && Directory.EnumerateFileSystemEntries(dest).Any());
            if (s.ExitCode != 0 || !exists || (s.Failed ?? 0) > 0)
                throw new JobException($"Backup failed: {s.Reason}");
            // MakeMKV writes an ISO image when the destination ends in .iso. If it wrote a folder instead, the backup is
            // complete but isn't an image: keep it as a folder backup rather than failing the job.
            if (_job.Drive.Rip.BackupFormat == BackupFormat.Iso && Directory.Exists(dest) && BackupVerifier.Problem(dest, false) == null)
            {
                var folder = Paths.UniquePath(Path.Combine(Path.GetDirectoryName(dest)!, Path.GetFileNameWithoutExtension(dest)));
                try
                {
                    Directory.Move(dest, folder);
                    _job.AppendLog($"MakeMKV wrote a folder instead of an ISO image; kept the backup as the folder {Path.GetFileName(folder)}", Severity.Warning);
                    dest = folder;
                }
                catch (IOException) { }
            }
            if (_job.Drive.Archive.VerifyRips && BackupVerifier.Problem(dest, !Directory.Exists(dest)) is { } problem)
                throw new JobException($"Backup failed the check: {problem}");
        }
        finally { _expectedDrive = null; }
        _job.AppendLog($"Backup saved to {dest}");
        // Without a disc listing a UHD disc looks like a plain Blu-ray; the backup's index.bdmv tells them apart.
        if (Directory.Exists(dest) && DiscFormatExtensions.DetectBackupFolder(dest) is { } found && found != _job.Identity?.Format)
        {
            ResolveIdentity(_job.DiscInfo, 0, found);
            var renamed = BackupName(decrypt);
            if (renamed.Length > 0 && renamed != Path.GetFileName(dest))
            {
                var target = Paths.UniquePath(Path.Combine(Path.GetDirectoryName(dest)!, renamed));
                try
                {
                    Directory.Move(dest, target);
                    _job.AppendLog($"Renamed the backup to {Path.GetFileName(target)}");
                    return target;
                }
                catch (IOException) { }
            }
        }
        return dest;
    }

    /// <summary>File / folder name of a backup from the file name template (rip = Backup, no episode or track).</summary>
    string BackupName(bool decrypt)
    {
        var template = _job.Drive.Output.Layout == LibraryLayout.MediaServer ? MediaServerNaming.BackupName : _job.Drive.Output.FileNameTemplate.Trim();
        if (template.Length == 0) return "";
        var values = TemplateValues(JobState.Running);
        values["rip"] = "Backup";
        if (_job.Identity is { } id) values["format"] = id.Format.Code(!decrypt);
        foreach (var k in new[] { "title", "index", "n", "source", "duration", "chapters", "original", "comment" }) values[k] = "";
        return TemplateRenderer.RenderPath(template, values).Replace(Path.DirectorySeparatorChar.ToString(), " - ");
    }

    /// <summary>A listing that holds exactly <paramref name="indices"/>, read with a minimum length that leaves out every
    /// other title, so they can be ripped in one run. Returns null (rip title by title) when that isn't possible or
    /// worthwhile: fewer than three titles, hand-picked tracks, or a title left out that is as long as a chosen one.</summary>
    async Task<(DiscInfo Info, int MinLength)?> OnePassListingAsync(DiscSource source, DiscInfo info, List<int> indices)
    {
        if (_job.TrackSelections.Count > 0 || OnePass.MinimumLength(indices, info, _job.Drive.Rip.MinLengthSeconds) is not { } minLength) return null;
        var rip = _job.Drive.Rip.Clone();
        rip.MinLengthSeconds = minLength;
        _job.Phase = "Reading the disc listing for a one-pass rip";
        var s = await RunMakeMkvAsync(_env.InfoArguments(source, rip));
        var listing = s.Info.Info;
        if (!OnePass.Matches(listing, indices, info))
        {
            _job.AppendLog($"A minimum length of {minLength} s doesn't leave exactly the chosen titles; ripping them one by one", Severity.Warning);
            return null;
        }
        ApplyTitleMap(info, listing);
        _job.AppendLog($"Ripping {indices.Count} titles in one pass (a minimum title length of {minLength} s leaves out the other {info.Titles.Count - indices.Count})");
        return (listing, minLength);
    }

    /// <summary>Fails the job before ripping when the destination can't hold the chosen titles (sizes from the listing),
    /// plus a copy of titles with hand-picked tracks (remuxed) and of a "play all" title (split into episodes).</summary>
    void CheckFreeSpace(List<TitleInfo> titles, string outDir)
    {
        long need = titles.Sum(t => t.SizeBytes) + titles.Where(t => _job.TrackSelections.ContainsKey(t.Index)).Sum(t => t.SizeBytes);
        if (_episodePlan is { } plan && titles.FirstOrDefault(t => t.SourceTitleId == plan.Title) is { } playAll) need += playAll.SizeBytes;
        if (need <= 0 || DiskSpace.Available(outDir) is not { } free) return;
        long required = DiskSpace.Required(need);
        _job.AppendLog($"Free space: {TitleInfo.FormatBytes(free)}; the titles need about {TitleInfo.FormatBytes(need)}");
        if (free < required)
            throw new JobException($"Not enough free space in {Path.GetDirectoryName(outDir)}: the titles need about {TitleInfo.FormatBytes(required)} (with a margin), only {TitleInfo.FormatBytes(free)} is free.");
    }

    /// <summary>Checks a ripped file against its title in the disc listing. Returns why it can't be trusted, or null.</summary>
    async Task<string?> VerifyAsync(string file, TitleInfo title)
    {
        if (!_job.Drive.Archive.VerifyRips) return null;
        var name = Path.GetFileName(file);
        if (_mkvmerge == null)
        {
            if (!_reportedMissingVerifier)
            {
                _reportedMissingVerifier = true;
                _job.AppendLog("Ripped files can't be checked against the disc listing without mkvmerge (MKVToolNix)", Severity.Warning);
            }
            return null;
        }
        _job.Phase = $"Checking {name}";
        var probe = await RipVerifier.ProbeAsync(_mkvmerge, file, _cts.Token);
        if (probe == null) return CancelRequested ? null : $"mkvmerge can't read {name}";
        var result = RipVerifier.Check(probe, title);
        foreach (var n in result.Notes) _job.AppendLog($"{name}: {n}", Severity.Warning);
        if (result.Problems.Count > 0) return $"{name} doesn't match the disc listing: {string.Join("; ", result.Problems)}";
        var length = probe.DurationSeconds is { } d ? TitleInfo.FormatDuration((int)Math.Round(d)) : "?";
        _job.AppendLog($"Checked {name}: {length}, {probe.TrackTypes.Count} track(s), {probe.ChapterCount} chapter(s)");
        if (title.Tracks.Count > 0 && probe.TrackTypes.Count < title.Tracks.Count)
            _job.AppendLog($"{name} has {probe.TrackTypes.Count} of the {title.Tracks.Count} tracks on the disc; the selection rule left out the rest (use “+sel:all” to keep every track)");
        return null;
    }

    async Task<string> RemuxAsync(string file, TitleInfo title, HashSet<int> keep)
    {
        if (_mkvmerge == null || keep.Count == title.Tracks.Count) return file;
        _job.Phase = "Removing unselected tracks";
        var layout = await Remuxer.IdentifyAsync(_mkvmerge, file);
        if (layout == null)
        {
            _job.AppendLog($"mkvmerge could not read {Path.GetFileName(file)}; keeping all tracks", Severity.Warning);
            return file;
        }
        var tmp = Path.Combine(Path.GetDirectoryName(file)!, $".bromelia-{Guid.NewGuid().ToString("N")[..8]}.mkv");
        var args = Remuxer.Arguments(layout, title, keep, file, tmp);
        if (args == null)
        {
            _job.AppendLog($"Track layout of {Path.GetFileName(file)} differs from the disc listing; keeping all tracks", Severity.Warning);
            return file;
        }
        var runner = new ProcessRunner(_mkvmerge, args);
        _job.AppendLog("$ " + runner.CommandLine);
        _active = runner;
        ProcessRunner.Result? r = null;
        try { r = await runner.RunAsync(_ => { }); }
        catch (Exception e) { _job.AppendLog($"mkvmerge failed: {e.Message}", Severity.Warning); }
        finally { _active = null; }
        // mkvmerge exits with 1 for warnings; the file is still valid.
        if (r is { ExitCode: <= 1 } && File.Exists(tmp))
        {
            File.Move(tmp, file, overwrite: true);
            _job.AppendLog($"Kept {keep.Count} of {title.Tracks.Count} tracks in {Path.GetFileName(file)}");
        }
        else
        {
            try { File.Delete(tmp); } catch (IOException) { }
            _job.AppendLog($"mkvmerge failed; keeping all tracks in {Path.GetFileName(file)}", Severity.Warning);
        }
        return file;
    }

    string Rename(string file, TitleInfo title, int ordinal, DiscInfo info, string outDir)
    {
        var explicitName = _job.TitleNameOverrides.TryGetValue(title.Index, out var o) ? o.Trim() : "";
        var values = TemplateValues(JobState.Running);
        foreach (var kv in TitleValues(title, ordinal, info, file)) values[kv.Key] = kv.Value;
        values["track"] = MediaIdentity.TrackLabel(title);
        bool episode = _separateEpisodes.TryGetValue(title.Index, out var k);
        if (episode) SetEpisode(_firstEpisode + k, values);
        var template = explicitName.Length > 0 ? explicitName
            : _job.Drive.Output.Layout == LibraryLayout.MediaServer ? MediaServerNaming.FileTemplate(values, title.Index == _mainTitle)
            : _job.Drive.Output.FileNameTemplate.Trim();
        var final = RenameTo(file, values, template, null, outDir);
        if (episode)
            _job.Episodes.Add(new ArchiveRecord.EpisodeEntry { File = RelativePath(final, outDir), Episode = _firstEpisode + k,
                SourceTitleId = title.SourceTitleId ?? title.Index, FirstChapter = 1, LastChapter = Math.Max(1, title.ChapterCount),
                Title = _episodeDetails.TryGetValue(_firstEpisode + k, out var ed) ? ed.Title : null });
        return final;
    }

    /// <summary>Moves <paramref name="file"/> to the name rendered from <paramref name="template"/>. With an empty
    /// template the file keeps its name, or gets <paramref name="fallbackName"/> (split episodes).</summary>
    string RenameTo(string file, Dictionary<string, string> values, string template, string? fallbackName, string outDir)
    {
        var rel = template.Trim().Length == 0 ? "" : TemplateRenderer.RenderPath(template, values);
        if (rel.Length == 0 && fallbackName != null) rel = TemplateRenderer.SanitizeComponent(fallbackName);
        if (rel.Length == 0) return file;
        if (!rel.EndsWith(".mkv", StringComparison.OrdinalIgnoreCase)) rel += ".mkv";
        var target = Path.Combine(outDir, rel);
        if (string.Equals(Path.GetFullPath(target), Path.GetFullPath(file), StringComparison.OrdinalIgnoreCase)) return file;
        target = Paths.UniquePath(target);
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            File.Move(file, target);
            _job.AppendLog($"Renamed {Path.GetFileName(file)} → {rel}");
            return target;
        }
        catch (IOException e)
        {
            _job.AppendLog($"Could not rename {Path.GetFileName(file)}: {e.Message}", Severity.Warning);
            return file;
        }
    }

    string ResolveOutputDirectory()
    {
        var root = Paths.ExpandUser(_config.OutputRootFor(_job.Drive));
        if (root.Length == 0) root = Paths.DefaultOutputRoot;
        bool mediaServer = _job.Drive.Output.Layout == LibraryLayout.MediaServer;
        var rel = TemplateRenderer.RenderPath(mediaServer ? MediaServerNaming.FolderTemplate : _job.Drive.Output.FolderTemplate, TemplateValues(JobState.Running));
        var dir = rel.Length == 0 ? root : Path.Combine(root, rel);
        // A media server library keeps one folder per movie / show: later discs are added to it.
        if (mediaServer)
        {
            Directory.CreateDirectory(dir);
            _ownsOutputDir = false;
            return dir;
        }
        // A staging folder of a running job counts as content, so two jobs never both take a folder as new.
        bool occupied = Directory.Exists(dir) && Directory.EnumerateFileSystemEntries(dir)
            .Select(e => Path.GetFileName(e)!).Any(n => !n.StartsWith('.') || n.StartsWith(StagingPrefix, StringComparison.Ordinal));
        // Only a folder made for this disc may be renamed or removed; never the output root itself.
        bool owned = rel.Length > 0;
        if (occupied)
        {
            switch (_job.Drive.Output.ConflictPolicy)
            {
                case ConflictPolicy.UniqueSuffix: dir = Paths.UniquePath(dir); break;
                case ConflictPolicy.Overwrite: owned = false; break;
                case ConflictPolicy.Skip: throw new JobException($"Output folder {dir} already exists");
            }
        }
        Directory.CreateDirectory(dir);
        _ownsOutputDir = owned;
        return dir;
    }

    static string WriteDiscInfo(DiscInfo info, string dir)
    {
        var path = Path.Combine(dir, "disc-info.json");
        File.WriteAllText(path, DiscInfoExport.ToJson(info));
        return path;
    }

    static void TryWriteDiscInfo(DiscInfo info, string dir)
    {
        try { WriteDiscInfo(info, dir); } catch (IOException) { }
    }

    static HashSet<string> MkvFiles(string dir) =>
        Directory.Exists(dir)
            ? Directory.EnumerateFiles(dir, "*.mkv").Where(f => !Path.GetFileName(f).StartsWith('.')).ToHashSet(StringComparer.Ordinal)
            : new HashSet<string>();

    public static Dictionary<string, string> TitleValues(TitleInfo t, int ordinal, DiscInfo disc, string? originalFile)
    {
        int d = t.DurationSeconds;
        return new Dictionary<string, string>
        {
            ["title"] = t.Name.Length == 0 ? disc.Name : t.Name,
            ["index"] = t.Index.ToString(CultureInfo.InvariantCulture),
            ["n"] = ordinal.ToString(CultureInfo.InvariantCulture),
            ["source"] = t.SourceTitleId?.ToString(CultureInfo.InvariantCulture) ?? "",
            ["duration"] = string.Format(CultureInfo.InvariantCulture, "{0}-{1:00}-{2:00}", d / 3600, d / 60 % 60, d % 60),
            ["chapters"] = t.ChapterCount.ToString(CultureInfo.InvariantCulture),
            ["original"] = originalFile != null ? Path.GetFileNameWithoutExtension(originalFile) : Path.GetFileNameWithoutExtension(t.OutputFileName),
            ["comment"] = t.Comment,
        };
    }

    public Dictionary<string, string> TemplateValues(JobState status)
    {
        var v = TemplateRenderer.DateValues(_job.StartedAt ?? DateTime.Now);
        var info = _job.DiscInfo;
        var disc = new[] { info?.Name, _job.DiscLabel, info?.VolumeName }.FirstOrDefault(s => !string.IsNullOrEmpty(s)) ?? "Disc";
        v["disc"] = disc;
        v["volume"] = info?.VolumeName ?? _job.DiscLabel;
        v["type"] = info?.TypeToken ?? "disc";
        v["drive"] = _job.Drive.Name;
        v["job"] = _job.Id.ToString("N")[..8];
        v["device"] = _job.Source is DiscSource.Drive d ? d.DevicePath : "";
        v["outputDir"] = _job.OutputDirectory ?? "";
        v["status"] = status.StatusWord();
        v["manifest"] = _job.ManifestFile;
        v["file"] = _job.ProducedFiles.FirstOrDefault() ?? "";
        v["files"] = string.Join(" ", _job.ProducedFiles);
        var id = _job.Identity ?? MediaIdentity.Resolve(info, _job.DiscLabel, _job.Mode == RipMode.Backup, _job.DiscFlags, null, _job.MediaName, _job.MediaKind);
        foreach (var kv in id.TemplateValues(_job.Mode.MakesMkv() ? "Rip" : "Backup")) v[kv.Key] = kv.Value;
        v["checksums"] = _job.ChecksumFile ?? "";
        v["releaseYear"] = _job.Metadata?.Year?.ToString(CultureInfo.InvariantCulture) ?? "";
        v["tmdb"] = _job.Metadata?.TmdbId?.ToString(CultureInfo.InvariantCulture) ?? "";
        v["imdb"] = _job.Metadata?.ImdbId ?? "";
        v["libraryFolder"] = id.Kind == MediaKind.Tv ? "TV Shows" : "Movies";
        v["seasonOr1"] = id.Label.Season?.ToString(CultureInfo.InvariantCulture) ?? "1";
        return v;
    }

    /// <summary>The drive's steps followed by the global plugins that apply to this status and disc.</summary>
    List<PostProcessStep> ApplicableSteps(JobState status)
    {
        var id = _job.Identity;
        return _job.Drive.PostProcess.Concat(_config.Plugins)
            .Where(s => PostProcessor.ShouldRun(s, status) && PluginMatcher.Matches(s, id?.Name ?? _job.DiscLabel, _job.DiscLabel, id?.FormatCode ?? ""))
            .ToList();
    }

    Dictionary<string, string> ScriptEnvironment(JobState status)
    {
        var e = new Dictionary<string, string>
        {
            ["BROMELIA_JOB_ID"] = _job.Id.ToString(),
            ["BROMELIA_STATUS"] = status.StatusWord(),
            ["BROMELIA_MODE"] = JsonNamingPolicy.CamelCase.ConvertName(_job.Mode.ToString()),
            ["BROMELIA_DRIVE_NAME"] = _job.Drive.Name,
            ["BROMELIA_DRIVE_ID"] = _job.Drive.Id.ToString(),
            ["BROMELIA_DISC_NAME"] = _job.DiscLabel,
            ["BROMELIA_DISC_TYPE"] = _job.DiscInfo?.TypeToken ?? "disc",
            ["BROMELIA_OUTPUT_DIR"] = _job.OutputDirectory ?? "",
            ["BROMELIA_FILES"] = string.Join("\n", _job.ProducedFiles),
            ["BROMELIA_FILE_COUNT"] = _job.ProducedFiles.Count.ToString(CultureInfo.InvariantCulture),
            ["BROMELIA_MANIFEST"] = _job.ManifestFile,
            ["BROMELIA_LOG"] = _job.LogFile,
            ["BROMELIA_SOURCE"] = _job.Source.InfoArgument,
        };
        if (_job.Source is DiscSource.Drive d) e["BROMELIA_DEVICE"] = d.DevicePath;
        if (_job.ErrorMessage != null) e["BROMELIA_ERROR"] = _job.ErrorMessage;
        if (_job.Identity is { } id)
        {
            e["BROMELIA_NAME"] = id.Name;
            e["BROMELIA_KIND"] = id.Kind.Token();
            e["BROMELIA_FORMAT"] = id.FormatCode;
            e["BROMELIA_ENCRYPTED"] = id.Encrypted ? "1" : "0";
            e["BROMELIA_SEASON"] = id.Label.Season?.ToString(CultureInfo.InvariantCulture) ?? "";
            e["BROMELIA_DISC_NUMBER"] = id.Label.Disc?.ToString(CultureInfo.InvariantCulture) ?? "";
            e["BROMELIA_DISC_SET"] = id.Label.SetDescription;
        }
        if (_job.ChecksumFile != null) e["BROMELIA_CHECKSUMS"] = _job.ChecksumFile;
        return e;
    }

    void WriteManifest(JobState status)
    {
        var info = _job.RipInfo ?? _job.DiscInfo;
        var m = new JobManifest
        {
            JobId = _job.Id.ToString(),
            Status = status.StatusWord(),
            Mode = JsonNamingPolicy.CamelCase.ConvertName(_job.Mode.ToString()),
            DriveName = _job.Drive.Name,
            DriveId = _job.Drive.Id.ToString(),
            DevicePath = _job.Source is DiscSource.Drive d ? d.DevicePath : "",
            Source = _job.Source.InfoArgument,
            DiscName = _job.DiscLabel,
            DiscType = info?.TypeToken ?? "disc",
            OutputDirectory = _job.OutputDirectory ?? "",
            Files = _job.ProducedFiles.ToList(),
            Titles = _job.RipTitles.Select(i => info?.Title(i)).Where(t => t != null).Select(t => new JobManifest.TitleEntry
            {
                Index = t!.Index, Name = t.Name, Duration = t.DurationText, Chapters = t.ChapterCount, SourceTitleId = t.SourceTitleId,
            }).ToList(),
            Name = _job.Identity?.Name ?? "",
            Kind = _job.Identity?.Kind.Token() ?? "",
            Format = _job.Identity?.Format.Token() ?? "",
            FormatCode = _job.Identity?.FormatCode ?? "",
            Encrypted = _job.Identity?.Encrypted ?? false,
            Season = _job.Identity?.Label.Season,
            DiscNumber = _job.Identity?.Label.Disc,
            Episodes = _job.Episodes.ToList(),
            ChecksumFile = _job.ChecksumFile,
            Checksums = _job.Checksums,
            StartedAt = _job.StartedAt,
            FinishedAt = _job.FinishedAt,
            Error = _job.ErrorMessage,
        };
        try
        {
            Directory.CreateDirectory(_job.LogDirectory);
            File.WriteAllText(_job.ManifestFile, ConfigJson.Serialize(m));
        }
        catch (IOException) { }
    }
}

/// <summary>Stable JSON for disc-info.json (attribute names instead of numeric ids).</summary>
public static class DiscInfoExport
{
    static Dictionary<string, string> Named(Dictionary<int, string> a) =>
        a.ToDictionary(kv => Enum.IsDefined(typeof(AttributeId), kv.Key) ? JsonNamingPolicy.CamelCase.ConvertName(((AttributeId)kv.Key).ToString()) : $"attr{kv.Key}", kv => kv.Value);

    public static string ToJson(DiscInfo info) => JsonSerializer.Serialize(new
    {
        attributes = Named(info.Attributes),
        titles = info.Titles.Select(t => new
        {
            index = t.Index,
            attributes = Named(t.Attributes),
            tracks = t.Tracks.Select(s => new { index = s.Index, attributes = Named(s.Attributes) }),
        }),
    }, new JsonSerializerOptions { WriteIndented = true });
}
