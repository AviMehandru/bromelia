using System.Globalization;
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
    Task<bool> EjectAsync(string devicePath);
    void Notify(string title, string body, bool sound);
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
    readonly Dictionary<string, int> _fileTitles = new(StringComparer.Ordinal);

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

        WriteManifest(status);
        var steps = ApplicableSteps(status);
        if (steps.Count > 0 && !CancelRequested)
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
            ((status == JobState.Succeeded && _job.Drive.Automation.EjectWhenDone) || (status == JobState.Failed && _job.Drive.Automation.EjectOnFailure)))
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
        _job.AppendLog($"Finished: {status.Label()} in {FormatElapsed(_job.Elapsed)}");
        _job.CloseLogFile();
        _videoTs?.Dispose();
        if (_job.Drive.Archive.ArchiveRecord && _job.OutputDirectory is { } archiveDir && _job.ProducedFiles.Count > 0)
        {
            try { File.Copy(_job.LogFile, Paths.UniquePath(Path.Combine(archiveDir, "bromelia-log.txt"))); } catch (IOException) { } catch (UnauthorizedAccessException) { }
        }

        if (_job.Drive.Automation.Notify)
        {
            var body = status switch
            {
                JobState.Succeeded => $"{_job.ProducedFiles.Count} item(s) saved to {_job.OutputDirectory}",
                JobState.Cancelled => "Cancelled",
                _ => _job.ErrorMessage ?? "Failed",
            };
            var what = _job.DiscLabel.Length == 0 ? _job.SourceLabel : _job.DiscLabel;
            _platform.Notify($"{_job.Mode.ShortLabel()} {(status == JobState.Succeeded ? "finished" : status.Label().ToLowerInvariant())}: {what}",
                body, _job.Drive.Automation.PlaySound);
        }
        Finished?.Invoke(_job);
    }

    public static string FormatElapsed(TimeSpan t) =>
        t.TotalHours >= 1 ? t.ToString(@"h\:mm\:ss", CultureInfo.InvariantCulture) : t.ToString(@"m\:ss", CultureInfo.InvariantCulture);

    // --- pipeline ----------------------------------------------------------------------------

    async Task ExecuteAsync()
    {
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

        var info = _job.PreloadedInfo;
        if (info == null)
        {
            if (_job.Mode.MakesMkv() || _job.Mode == RipMode.InfoOnly) info = await ScanDiscAsync(_job.Source);
            else
            {
                // Backups don't need the listing, but it tells DVD, Blu-ray and 4K UHD apart and gives the title.
                try { info = await ScanDiscAsync(_job.Source); }
                catch (JobException e) { _job.AppendLog($"Continuing without the disc listing: {e.Message}", Severity.Warning); }
            }
        }
        _job.DiscInfo = info;
        if (info is { Name.Length: > 0 }) _job.DiscLabel = info.Name;
        ResolveIdentity(info);

        var outDir = ResolveOutputDirectory();
        _job.OutputDirectory = outDir;
        _job.AppendLog($"Output folder: {outDir}");

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
            case RipMode.BackupThenMkv:
                _job.StepCount = 2;
                var dest = await BackupAsync(true, outDir, true);
                _job.StepIndex = 1;
                DiscSource backupSource = _job.Drive.Rip.BackupFormat == BackupFormat.Iso ? new DiscSource.Iso(dest) : new DiscSource.Folder(dest);
                var backupInfo = await ScanDiscAsync(backupSource);
                RemapSelections(info, backupInfo);
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
        await ArchiveAsync(outDir);
    }

    // --- identity ----------------------------------------------------------------------------------

    void ResolveIdentity(DiscInfo? info, int playAllEpisodes = 0, DiscFormat? format = null)
    {
        var id = MediaIdentity.Resolve(info, _job.DiscLabel, _job.Mode == RipMode.Backup, _job.DiscFlags, format ?? _job.Identity?.Format,
            _job.MediaName, _job.MediaKind, playAllEpisodes);
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
        ResolveIdentity(info, plans.FirstOrDefault(p => p.IsPlausible(true))?.EpisodeCount ?? 0);
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
        if (first == null) how = $"numbered from 1 ({how})";
        _firstEpisode = first ?? 1;
        _episodeWidth = Math.Max(2, (_firstEpisode + count - 1).ToString(CultureInfo.InvariantCulture).Length);
        _job.AppendLog($"Episodes {_firstEpisode}–{_firstEpisode + count - 1} ({how})");
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
                values["episode"] = MediaIdentity.EpisodeLabel(_firstEpisode + i, _episodeWidth);
                values["episodeNumber"] = (_firstEpisode + i).ToString(CultureInfo.InvariantCulture);
            }
            else (firstCh, lastCh) = (plan.Tail[0], plan.Tail[^1]);
            values["track"] = $"{MediaIdentity.TrackLabel(title)} Ch {(firstCh == lastCh ? $"{firstCh}" : $"{firstCh}-{lastCh}")}";
            var fallback = $"{Path.GetFileNameWithoutExtension(file)} - {(i < plan.EpisodeCount ? $"Episode {_firstEpisode + i}" : "after last episode")}";
            var final = RenameTo(parts[i], values, _job.Drive.Output.FileNameTemplate, fallback, outDir);
            outputs.Add(final);
            if (i < plan.EpisodeCount)
                _job.Episodes.Add(new ArchiveRecord.EpisodeEntry { File = RelativePath(final, outDir), Episode = _firstEpisode + i, SourceTitleId = plan.Title, FirstChapter = firstCh, LastChapter = lastCh });
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

    /// <summary>Hashes every produced file and writes SHA256SUMS and bromelia.json into the output folder.</summary>
    async Task ArchiveAsync(string outDir)
    {
        var cfg = _job.Drive.Archive;
        if (!(cfg.Checksums || cfg.ArchiveRecord) || _job.ProducedFiles.Count == 0) return;
        var files = Checksums.Files(_job.ProducedFiles, outDir);
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
        _job.Checksums = entries;
        if (cfg.Checksums)
        {
            try
            {
                _job.ChecksumFile = Checksums.WriteMerged(outDir, entries);
                _job.AppendLog($"Wrote SHA-256 checksums of {entries.Count} file(s) to {Checksums.FileName}");
            }
            catch (IOException e) { throw new JobException($"Could not write {Path.Combine(outDir, Checksums.FileName)}: {e.Message}"); }
        }
        if (cfg.ArchiveRecord) WriteArchiveRecord(outDir);
    }

    void WriteArchiveRecord(string outDir)
    {
        if (_job.Identity is not { } id) return;
        var info = _job.DiscInfo;
        var record = new ArchiveRecord
        {
            Name = id.Name,
            Kind = id.Kind.Token(),
            Disc = new ArchiveRecord.DiscEntry
            {
                Label = _job.DiscLabel, VolumeName = info?.VolumeName ?? "", Type = info?.TypeName ?? "", Format = id.Format.Token(),
                FormatCode = id.FormatCode, Encrypted = id.Encrypted, Season = id.Label.Season, Part = id.Label.Part, Volume = id.Label.Volume, Disc = id.Label.Disc,
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
        };
        var path = Paths.UniquePath(Path.Combine(outDir, "bromelia.json"));
        try
        {
            File.WriteAllText(path, record.ToJson());
            _job.AppendLog($"Wrote the archive record {Path.GetFileName(path)}");
        }
        catch (IOException e) { _job.AppendLog($"Could not write {path}: {e.Message}", Severity.Warning); }
    }

    async Task<RunSummary> RunMakeMkvAsync(List<string> args)
    {
        if (CancelRequested) throw new OperationCanceledException();
        var summary = new RunSummary();
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
                }, default, _cts.Token);
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
        summary.ExitCode = result.ExitCode;
        if (CancelRequested || result.Cancelled) throw new OperationCanceledException();
        if (summary.DriveMismatch != null) throw new JobException(summary.DriveMismatch);
        return summary;
    }

    void Handle(RobotEvent ev, RunSummary s, bool showDebug)
    {
        s.Info.Consume(ev);
        switch (ev)
        {
            case RobotEvent.Message { Value: var m }:
                var sev = m.Severity;
                if (sev == Severity.Debug && !showDebug) return;
                _job.AppendLog(m.Text, sev);
                if (sev == Severity.Error) { s.ErrorMessages.Add(m.Text); _job.ErrorMessages.Add(m.Text); }
                if (m.Code == 1005 && _job.MakemkvVersion.Length == 0) _job.MakemkvVersion = m.Parameters.Count > 0 ? m.Parameters[0] : m.Text;
                if (m.Code is 5036 or 5005 && m.Parameters.Count > 0 && int.TryParse(m.Parameters[0], out var saved)) s.Saved = saved;
                if (m.Code == 5037 && m.Parameters.Count > 1)
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

    async Task RipTitlesAsync(DiscSource source, DiscInfo info, string outDir, int extraSteps)
    {
        var indices = ChooseTitles(info);
        _job.RipTitles = indices;
        _fileTitles.Clear();
        await PrepareEpisodesAsync(source, info, indices);
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
            try { s = await RunMakeMkvAsync(_env.MkvArguments(source, t, outDir, _job.Drive.Rip)); }
            finally { _env = _baseEnv; }
            var produced = MkvFiles(outDir).Except(before).OrderBy(p => p, StringComparer.Ordinal).ToList();
            if (s.ExitCode != 0 || (s.Failed ?? 0) > 0 || produced.Count == 0)
            {
                var why = s.ErrorMessages.LastOrDefault() ?? $"makemkvcon exit status {s.ExitCode}";
                failures.Add(single ? why : $"title {t}: {why}");
                _job.AppendLog($"Title {t} failed: {why}", Severity.Error);
                if (single && produced.Count == 0) break;
            }
            foreach (var file in produced)
            {
                int? titleIndex = single ? info.Titles.FirstOrDefault(x => x.OutputFileName == Path.GetFileName(file))?.Index : tIndex;
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
        if (inSubfolder)
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
            var s = await RunMakeMkvAsync(args);
            bool exists = File.Exists(dest) || (Directory.Exists(dest) && Directory.EnumerateFileSystemEntries(dest).Any());
            if (s.ExitCode != 0 || !exists || (s.Failed ?? 0) > 0)
                throw new JobException($"Backup failed: {s.ErrorMessages.LastOrDefault() ?? $"makemkvcon exit status {s.ExitCode}"}");
        }
        finally { _expectedDrive = null; }
        _job.AppendLog($"Backup saved to {dest}");
        // Without a disc listing a UHD disc looks like a plain Blu-ray; the backup's index.bdmv tells them apart.
        if (_job.Drive.Rip.BackupFormat == BackupFormat.Folder && DiscFormatExtensions.DetectBackupFolder(dest) is { } found && found != _job.Identity?.Format)
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
        var template = _job.Drive.Output.FileNameTemplate.Trim();
        if (template.Length == 0) return "";
        var values = TemplateValues(JobState.Running);
        values["rip"] = "Backup";
        if (_job.Identity is { } id) values["format"] = id.Format.Code(!decrypt);
        foreach (var k in new[] { "title", "index", "n", "source", "duration", "chapters", "original", "comment" }) values[k] = "";
        return TemplateRenderer.RenderPath(template, values).Replace(Path.DirectorySeparatorChar.ToString(), " - ");
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
        var template = explicitName.Length > 0 ? explicitName : _job.Drive.Output.FileNameTemplate.Trim();
        var values = TemplateValues(JobState.Running);
        foreach (var kv in TitleValues(title, ordinal, info, file)) values[kv.Key] = kv.Value;
        values["track"] = MediaIdentity.TrackLabel(title);
        bool episode = _separateEpisodes.TryGetValue(title.Index, out var k);
        if (episode)
        {
            values["episode"] = MediaIdentity.EpisodeLabel(_firstEpisode + k, _episodeWidth);
            values["episodeNumber"] = (_firstEpisode + k).ToString(CultureInfo.InvariantCulture);
        }
        var final = RenameTo(file, values, template, null, outDir);
        if (episode)
            _job.Episodes.Add(new ArchiveRecord.EpisodeEntry { File = RelativePath(final, outDir), Episode = _firstEpisode + k,
                SourceTitleId = title.SourceTitleId ?? title.Index, FirstChapter = 1, LastChapter = Math.Max(1, title.ChapterCount) });
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

    void RemapSelections(DiscInfo? disc, DiscInfo backup)
    {
        if (disc == null) return;
        static string Key(TitleInfo t) => $"{t.SourceTitleId ?? -1}|{t.DurationSeconds}|{t.SegmentMap}";
        var map = new Dictionary<int, int>();
        foreach (var t in disc.Titles)
        {
            var b = backup.Titles.FirstOrDefault(x => Key(x) == Key(t)) ?? backup.Title(t.Index);
            if (b != null) map[t.Index] = b.Index;
        }
        if (_job.ManualTitles is { } manual) _job.ManualTitles = manual.Where(map.ContainsKey).Select(i => map[i]).ToList();
        _job.TrackSelections = _job.TrackSelections.Where(kv => map.ContainsKey(kv.Key)).ToDictionary(kv => map[kv.Key], kv => kv.Value);
        _job.TitleNameOverrides = _job.TitleNameOverrides.Where(kv => map.ContainsKey(kv.Key)).ToDictionary(kv => map[kv.Key], kv => kv.Value);
    }

    string ResolveOutputDirectory()
    {
        var root = Paths.ExpandUser(_config.OutputRootFor(_job.Drive));
        if (root.Length == 0) root = Paths.DefaultOutputRoot;
        var rel = TemplateRenderer.RenderPath(_job.Drive.Output.FolderTemplate, TemplateValues(JobState.Running));
        var dir = rel.Length == 0 ? root : Path.Combine(root, rel);
        bool nonEmpty = Directory.Exists(dir) && Directory.EnumerateFileSystemEntries(dir).Any(e => !Path.GetFileName(e).StartsWith('.'));
        if (nonEmpty)
        {
            switch (_job.Drive.Output.ConflictPolicy)
            {
                case ConflictPolicy.UniqueSuffix: dir = Paths.UniquePath(dir); break;
                case ConflictPolicy.Skip: throw new JobException($"Output folder {dir} already exists");
            }
        }
        Directory.CreateDirectory(dir);
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
        var info = _job.DiscInfo;
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
