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
        var steps = _job.Drive.PostProcess.Where(s => PostProcessor.ShouldRun(s, status)).ToList();
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
        bool needInfo = _job.Mode.MakesMkv() || _job.Mode == RipMode.InfoOnly || _job.DiscLabel.Length == 0;
        if (info == null && needInfo) info = await ScanDiscAsync(_job.Source);
        _job.DiscInfo = info;
        if (info is { Name.Length: > 0 }) _job.DiscLabel = info.Name;

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
                if (sev == Severity.Error) s.ErrorMessages.Add(m.Text);
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
                }
                _job.ProducedFiles.Add(final);
            }
        }
        _job.StepIndex = _job.StepCount;
        if (failures.Count > 0)
            throw new JobException(failures.Count == 1 ? $"Rip failed: {failures[0]}" : $"{failures.Count} titles failed: {string.Join("; ", failures)}");
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
        if (_job.Drive.Rip.BackupFormat == BackupFormat.Iso)
            dest = Paths.UniquePath(Path.Combine(dest, TemplateRenderer.SanitizeComponent(_job.DiscLabel.Length == 0 ? "disc" : _job.DiscLabel) + ".iso"));
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
        return dest;
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
        if (template.Length == 0) return file;
        var values = TemplateValues(JobState.Running);
        foreach (var kv in TitleValues(title, ordinal, info, file)) values[kv.Key] = kv.Value;
        var rel = TemplateRenderer.RenderPath(template, values);
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
        return v;
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
