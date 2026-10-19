using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Engine;

/// <summary>Step 8 (plan §20.1, §12.4): makes what Plan decided in the job's staging folder (LibraryPort.stagingDirectory),
/// holding the drive, an acquisition slot and the library (LibraryWrite). It names, hashes and moves nothing.
/// <list type="bullet">
/// <item>Space: the plan's need is reserved again (reservations don't outlive the engine), less what an earlier attempt
/// kept.</item>
/// <item>infoOnly: disc-info.json, the listing Probe read (Listing.encode; rip.discInfoWritten).</item>
/// <item>mkv: the plan's titles from the source (and disc-info.json under archive.writeDiscInfo). One makemkvcon run for
/// every title of the listing ("all"; rip.ripping), or with Plan's one-pass minimum length when a listing read with it
/// holds exactly the titles (rip.onePassListing, then rip.onePass or rip.onePassMismatch); else title by title
/// (rip.rippingTitle), hand-picked tracks with +sel:all. A failed title (rip.titleFailed) leaves its files under
/// MakeMKV's names (rip.keptUnderMakemkvName) and the others are still ripped; the step then fails (rip.failed,
/// rip.titlesFailed) with the MakeMKV notice that explains it.</item>
/// <item>backup, backupDecrypted: the disc as a folder or an image (profile backup.format), named backup or backup.iso;
/// an image MakeMKV made for a folder gets .iso (backup.imageForFolder), a folder made for an image loses it
/// (backup.folderInsteadOfIso); backup.saved, or backup.failed.</item>
/// <item>backupThenMkv: a decrypted backup, its structure checked (backup.checkFailed), then the titles from the backup
/// (mapped from Probe's listing to the backup's), and the backup removed unless backup.keepAfterMkv (backup.removed).</item>
/// </list>
/// Read errors are counted per title (title by title) or per run (StepReport.readErrors); Release and Commit act on them.
/// A cancel removes what the stopped run wrote (rip.partialRemoved: makemkvcon leaves an unreadable file, P2-14) unless
/// the process was abandoned after KILL. After each title (its files synced) and after a checked backup, a checkpoint
/// holds the disc's fingerprint and what is finished: "backup" once a backup-then-MKV job has its backup (recovery
/// resumes from it by itself, plan §15.8: recovery.resumedFromBackup), else "titles". A later attempt of the step (a
/// retry or a continue) with the same disc keeps those titles (rip.continued) and removes anything else in staging
/// (rip.leftoverRemoved; owner 2026-10-18). Data: AcquireOutput.</summary>
public sealed class AcquireStep : IStep
{
    public const string TitlesCheckpoint = "titles";
    public const string BackupCheckpoint = "backup";
    public const string DiscInfoFile = "disc-info.json";

    public StepKind Kind => StepKind.Acquire;

    public Resumability Resumability => Resumability.ResumeFrom;

    public string? ResumeCheckpoint => BackupCheckpoint;

    public ResourceSet Resources(JobSnapshot job) => ResourceSet.None with
    {
        Drives = DiscRequest.Decode(job.Request)?.Source is DiscSource.Drive d ? new[] { d.DriveId } : Array.Empty<string>(),
        Acquisition = true,
        LibraryWrites = PlanOutput.Of(job) is { LibraryId.Length: > 0 } plan ? new[] { plan.LibraryId } : Array.Empty<string>(),
    };

    public Task<StepReport> Run(StepContext context) => new Acquisition(context).Run();

    /// <summary>One run of the step.</summary>
    sealed class Acquisition
    {
        readonly StepContext _c;
        readonly PlanOutput _plan;
        readonly ProbeOutput _probe;
        readonly DiscRequest _request;
        readonly IFileSystem _fs;
        string _staging = "";
        readonly List<AcquiredTitle> _titles = new();
        readonly List<string> _others = new();
        readonly List<string> _readErrors = new();
        string? _backup;
        string? _discInfo;
        MakemkvNotice? _notice;

        public Acquisition(StepContext context)
        {
            _c = context;
            _plan = PlanOutput.Of(context.Job) ?? throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.InternalUnexpected),
                ("detail", JsonValue.Of("acquire without a plan"))));
            _probe = ProbeOutput.Decode(JobSnapshot.Output(context.Job, StepKind.Probe) ?? JsonValue.Null.Instance);
            _request = StepSources.Request(context.Job);
            _fs = context.Fs;
        }

        public async Task<StepReport> Run()
        {
            var library = _c.Library.Require(_plan.LibraryId);
            _staging = _c.Library.StagingDirectory(_plan.LibraryId, _c.Job.Id);

            // What an earlier attempt finished, with this disc.
            var saved = _c.Checkpoint.Saved;
            bool same = saved is JsonValue.Object && _probe.Fingerprint != null && saved["fingerprint"]?.AsString == _probe.Fingerprint;
            var kind = saved?["kind"]?.AsString;
            string? resumeBackup = null;
            if (same && _plan.Mode == RipMode.BackupThenMkv && kind == BackupCheckpoint && saved!["backup"]?.AsString is { Length: > 0 } b
                && _fs.Exists(_staging + "/" + b))
                resumeBackup = b;
            if (same && ((_plan.Mode == RipMode.Mkv && kind == TitlesCheckpoint) || (_plan.Mode == RipMode.BackupThenMkv && kind is TitlesCheckpoint or BackupCheckpoint)))
            {
                foreach (var t in (saved!["titles"]?.AsArray ?? new List<JsonValue>()).Select(AcquiredTitle.Decode))
                    if (_plan.Titles.Any(p => p.Index == t.Index) && t.Files.Count > 0 && t.Files.All(f => _fs.Exists(_staging + "/" + f)))
                        _titles.Add(t);
                _readErrors.AddRange((saved["readErrors"]?.AsArray ?? new List<JsonValue>()).Select(e => e.AsString ?? "")); // a backup's
            }
            var keep = _titles.SelectMany(t => t.Files).Append(resumeBackup).OfType<string>().ToHashSet(StringComparer.Ordinal);
            foreach (var e in _fs.List(_staging).OrderBy(e => e.Name, StringComparer.Ordinal))
            {
                if (keep.Contains(e.Name)) continue;
                FileTree.Remove(_fs, _staging + "/" + e.Name);
                _c.Log.Log(new BroMessage(MessageCode.RipLeftoverRemoved, Severity.Info, ("file", JsonValue.Of(e.Name))));
            }
            if (resumeBackup != null) _c.Log.Log(new BroMessage(MessageCode.RecoveryResumedFromBackup));
            if (_titles.Count > 0) _c.Log.Log(new BroMessage(MessageCode.RipContinued, Severity.Info, ("count", JsonValue.Of(_titles.Count))));

            if (_plan.SpaceNeed is { } need)
            {
                long kept = _titles.Sum(t => _probe.Listing?.Titles.FirstOrDefault(x => x.Index == t.Index)?.SizeBytes ?? 0);
                if (need - kept > 0)
                {
                    var volume = _fs.Volume(library.Path);
                    await _c.Space.Reserve(volume.Id, library.Path, SpaceEstimate.Required(new Bytes(need - kept)).Count, volume.FreeBytes);
                }
            }

            switch (_plan.Mode)
            {
                case RipMode.InfoOnly:
                    WriteDiscInfo(_probe.Listing ?? throw NoInformation());
                    return Done();
                case RipMode.Backup or RipMode.BackupDecrypted:
                {
                    var made = await Backup(_plan.Mode == RipMode.BackupDecrypted, 0, 1);
                    if (made.Report is { } stopped) return stopped;
                    _backup = made.Name;
                    return Done();
                }
                case RipMode.Mkv:
                {
                    var listing = _probe.Listing ?? throw NoInformation();
                    if (Profile("archive", "writeDiscInfo")?.AsBool == true) WriteDiscInfo(listing);
                    var source = StepSources.Makemkv(_request.Source, _c.DriveLookup);
                    return await Titles(source, listing, _plan.Titles.ToDictionary(t => t.Index, t => t.Index), TitlesCheckpoint, 0, 1) ?? Done();
                }
                case RipMode.BackupThenMkv:
                    return await BackupThenMkv(_probe.Listing ?? throw NoInformation(), resumeBackup);
                default:
                    throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.InternalUnexpected), ("detail", JsonValue.Of("mode " + EnumWire.Name(_plan.Mode)))));
            }
        }

        async Task<StepReport> BackupThenMkv(Listing listing, string? backup)
        {
            if (_plan.Titles.All(p => _titles.Any(t => t.Index == p.Index)))
            {
                _backup = backup;
                return Done();
            }
            double ripsFrom = 0;
            if (backup == null)
            {
                var made = await Backup(true, 0, 0.5);
                if (made.Report is { } stopped) return stopped;
                backup = made.Name!;
                if (BackupCheck.Problem(_fs, _staging + "/" + backup) is { } problem)
                    return Failed(new BroMessage(MessageCode.BackupCheckFailed, Severity.Error, ("problem", problem.ToJson())));
                FileTree.Sync(_fs, _staging + "/" + backup);
                _backup = backup;
                await Save(BackupCheckpoint);
                ripsFrom = 0.5;
            }
            _backup = backup;
            var path = _staging + "/" + backup;
            var source = SourceResolver.Source(path, _fs.Stat(path).IsDirectory);
            var reading = new BroMessage(MessageCode.DiscOpReading);
            _c.Progress.Report(ripsFrom, reading);
            var read = await _c.Tools.Makemkv.Listing(source, Invocation(null, null), Sink(reading, ripsFrom, 0), _c.Cancel);
            LogRun(read.Run);
            if (read.Run.Outcome.Status == StatusWord.Cancelled || _c.Cancel.IsCancelled) return Cancelled();
            if (read.Listing.Titles.Count == 0)
                return Failed(new BroMessage(MessageCode.DiscNoTitles, Severity.Error, ("source", JsonValue.Of(path)),
                    ("reason", (read.Run.Outcome.Error ?? new BroMessage(MessageCode.ProcessExitStatus, Severity.Error,
                        ("status", JsonValue.Of(read.Run.Outcome.ExitCode)))).ToJson())));
            var handPicked = _plan.Titles.Where(t => t.Tracks != null).Select(t => t.Index).ToHashSet();
            var map = ListingMatch.MapTitles(_plan.Titles.Select(t => t.Index), listing, read.Listing, handPicked);
            if (Profile("archive", "writeDiscInfo")?.AsBool == true) WriteDiscInfo(read.Listing);
            if (await Titles(source, read.Listing, map, BackupCheckpoint, ripsFrom, 1 - ripsFrom) is { } failed) return failed;
            if (Profile("backup", "keepAfterMkv")?.AsBool != false) return Done();
            _backup = null;
            await Save(TitlesCheckpoint); // the titles stand without the backup from here on
            _c.Log.Log(new BroMessage(MessageCode.BackupRemoved, Severity.Info, ("path", JsonValue.Of(path))));
            FileTree.Remove(_fs, path);
            return Done();
        }

        /// <summary>Rips the plan's titles that aren't done yet from <paramref name="source"/>, whose listing is
        /// <paramref name="listing"/> (<paramref name="map"/>: the plan's index → its index there). Null when every run
        /// succeeded; else the report the step ends with.</summary>
        async Task<StepReport?> Titles(MakemkvSource source, Listing listing, IReadOnlyDictionary<int, int> map, string checkpoint, double from,
            double width)
        {
            var wanted = _plan.Titles.Where(p => _titles.All(t => t.Index != p.Index)).ToList();
            if (wanted.Count == 0) return null;
            bool handPicked = wanted.Any(t => t.Tracks != null);
            var runListing = listing;
            var runMap = map.ToDictionary(kv => kv.Key, kv => kv.Value);
            int? minLength = null;
            if (_plan.OnePassMinLength is { } m && _titles.Count == 0 && !handPicked)
            {
                var operation = new BroMessage(MessageCode.RipOnePassListing);
                _c.Progress.Report(from, operation);
                var read = await _c.Tools.Makemkv.Listing(source, Invocation(m, null), Sink(operation, from, 0), _c.Cancel);
                LogRun(read.Run);
                if (read.Run.Outcome.Status == StatusWord.Cancelled || _c.Cancel.IsCancelled) return Cancelled();
                var indices = wanted.Select(t => map[t.Index]).ToList();
                if (OnePass.Matches(read.Listing, indices, listing))
                {
                    _c.Log.Log(new BroMessage(MessageCode.RipOnePass, Severity.Info, ("count", JsonValue.Of(indices.Count)), ("minLength", JsonValue.Of(m)),
                        ("others", JsonValue.Of(listing.Titles.Count - indices.Count))));
                    var pass = ListingMatch.MapTitles(indices, listing, read.Listing, new HashSet<int>());
                    foreach (var t in wanted) runMap[t.Index] = pass[map[t.Index]];
                    runListing = read.Listing;
                    minLength = m;
                }
                else _c.Log.Log(new BroMessage(MessageCode.RipOnePassMismatch, Severity.Warning, ("minLength", JsonValue.Of(m))));
            }
            bool single = !handPicked && wanted.Count == runListing.Titles.Count
                && runListing.Titles.All(x => wanted.Any(t => runMap[t.Index] == x.Index));
            var runs = single ? new List<List<TitleChoice>> { wanted } : wanted.Select(t => new List<TitleChoice> { t }).ToList();
            var failures = new List<(int? Title, BroMessage Reason)>();
            for (int n = 0; n < runs.Count; n++)
            {
                if (_c.Cancel.IsCancelled) return Cancelled();
                var group = runs[n];
                var title = single ? "all" : runMap[group[0].Index].ToString(CultureInfo.InvariantCulture);
                var operation = single
                    ? new BroMessage(MessageCode.RipRipping, Severity.Info, ("count", JsonValue.Of(group.Count)))
                    : new BroMessage(MessageCode.RipRippingTitle, Severity.Info, ("title", JsonValue.Of(group[0].Index)), ("n", JsonValue.Of(n + 1)),
                        ("of", JsonValue.Of(runs.Count)));
                double at = from + width * n / runs.Count;
                _c.Progress.Report(at, operation);
                var run = await _c.Tools.Makemkv.Rip(source, title, _staging,
                    Invocation(minLength, !single && group[0].Tracks != null ? "+sel:all" : null), Sink(operation, at, width / runs.Count), _c.Cancel);
                LogRun(run);
                var outcome = run.Outcome;
                if (outcome.Status == StatusWord.Cancelled || _c.Cancel.IsCancelled)
                {
                    RemovePartial(outcome.Produced, outcome.Abandoned);
                    return Cancelled();
                }
                if (outcome.Status == StatusWord.Failed)
                {
                    var reason = outcome.Error ?? new BroMessage(MessageCode.ProcessExitStatus, Severity.Error, ("status", JsonValue.Of(outcome.ExitCode)));
                    _c.Log.Log(new BroMessage(MessageCode.RipTitleFailed, Severity.Error,
                        ("title", JsonValue.Of(single ? "all" : group[0].Index.ToString(CultureInfo.InvariantCulture))), ("reason", reason.ToJson())));
                    foreach (var f in outcome.Produced)
                    {
                        _c.Log.Log(new BroMessage(MessageCode.RipKeptUnderMakemkvName, Severity.Warning, ("file", JsonValue.Of(f))));
                        _others.Add(f);
                    }
                    failures.Add((single ? null : group[0].Index, reason));
                    _notice ??= run.Notice;
                    continue;
                }
                var errors = outcome.ReadErrors.Select(e => e.Text).ToList();
                var made = new List<AcquiredTitle>();
                if (single)
                {
                    foreach (var t in group)
                    {
                        var name = runListing.Titles.First(x => x.Index == runMap[t.Index]).OutputFileName;
                        made.Add(new AcquiredTitle(t.Index, outcome.Produced.Where(f => name.Length > 0 && f == name).ToList(),
                            group.Count == 1 ? errors : new List<string>()));
                    }
                    _others.AddRange(outcome.Produced.Where(f => made.All(t => !t.Files.Contains(f))));
                    if (group.Count > 1) _readErrors.AddRange(errors);
                }
                else made.Add(new AcquiredTitle(group[0].Index, outcome.Produced.ToList(), errors));
                foreach (var f in outcome.Produced) _fs.SyncFile(_staging + "/" + f);
                _titles.AddRange(made);
                await Save(checkpoint);
            }
            if (failures.Count == 0) return null;
            BroMessage Reason((int? Title, BroMessage Reason) f) => f.Title is { } t
                ? new BroMessage(MessageCode.RipTitleReason, Severity.Info, ("title", JsonValue.Of(t.ToString(CultureInfo.InvariantCulture))),
                    ("reason", f.Reason.ToJson()))
                : f.Reason;
            return Failed(failures.Count == 1
                ? new BroMessage(MessageCode.RipFailed, Severity.Error, ("reason", Reason(failures[0]).ToJson()))
                : new BroMessage(MessageCode.RipTitlesFailed, Severity.Error, ("count", JsonValue.Of(failures.Count)),
                    ("reasons", JsonValue.Of(failures.Select(f => Reason(f).ToJson())))));
        }

        /// <summary>A backup into staging: its name there, or the report the step ends with.</summary>
        async Task<(string? Name, StepReport? Report)> Backup(bool decrypt, double from, double width)
        {
            bool iso = Profile("backup", "format")?.AsString == "iso";
            var name = iso ? "backup.iso" : "backup";
            var destination = _staging + "/" + name;
            var source = StepSources.Makemkv(_request.Source, _c.DriveLookup);
            var operation = new BroMessage(MessageCode.BackupBackingUp, Severity.Info, ("decrypted", JsonValue.Of(decrypt)));
            _c.Progress.Report(from, operation);
            var run = await _c.Tools.Makemkv.Backup(source, decrypt, destination, Invocation(null, null), Sink(operation, from, width), _c.Cancel);
            LogRun(run);
            var outcome = run.Outcome;
            if (outcome.Status == StatusWord.Cancelled || _c.Cancel.IsCancelled)
            {
                if (_fs.Exists(destination)) RemovePartial(new[] { name }, outcome.Abandoned);
                return (null, Cancelled());
            }
            if (outcome.Status == StatusWord.Failed)
            {
                _notice ??= run.Notice;
                var reason = outcome.Error ?? new BroMessage(MessageCode.ProcessExitStatus, Severity.Error, ("status", JsonValue.Of(outcome.ExitCode)));
                if (_fs.Exists(destination)) _others.Add(name);
                return (null, Failed(new BroMessage(MessageCode.BackupFailed, Severity.Error, ("reason", reason.ToJson()))));
            }
            bool folder = _fs.Stat(destination).IsDirectory;
            if (iso && folder)
            {
                name = Rename(name, "backup", true);
                _c.Log.Log(new BroMessage(MessageCode.BackupFolderInsteadOfIso, Severity.Warning, ("folder", JsonValue.Of(name))));
            }
            else if (!iso && !folder)
            {
                name = Rename(name, "backup.iso", false);
                _c.Log.Log(new BroMessage(MessageCode.BackupImageForFolder, Severity.Info, ("file", JsonValue.Of(name))));
            }
            _c.Log.Log(new BroMessage(MessageCode.BackupSaved, Severity.Info, ("path", JsonValue.Of(_staging + "/" + name))));
            _readErrors.AddRange(outcome.ReadErrors.Select(e => e.Text));
            return (name, null);
        }

        /// <summary>Renames a staging entry to a free name like <paramref name="wanted"/>; the new name.</summary>
        string Rename(string name, string wanted, bool isFolder)
        {
            var free = ConflictNamer.Next(wanted, _fs.List(_staging).Select(e => e.Name).ToList(), isFolder);
            _fs.Rename(_staging + "/" + name, _staging + "/" + free);
            return free;
        }

        /// <summary>What a stopped run wrote: unreadable (P2-14), so removed; kept when the process may still be writing.</summary>
        void RemovePartial(IReadOnlyList<string> names, bool abandoned)
        {
            if (abandoned) return;
            foreach (var name in names)
            {
                FileTree.Remove(_fs, _staging + "/" + name);
                _c.Log.Log(new BroMessage(MessageCode.RipPartialRemoved, Severity.Info, ("file", JsonValue.Of(name))));
            }
        }

        void WriteDiscInfo(Listing listing)
        {
            _fs.WriteAtomically(_staging + "/" + DiscInfoFile, JsonValue.EncodeCanonical(Listing.Encode(listing)), 0x1A4); // 0644
            _discInfo = DiscInfoFile;
            _c.Log.Log(new BroMessage(MessageCode.RipDiscInfoWritten, Severity.Info, ("file", JsonValue.Of(DiscInfoFile))));
        }

        /// <summary>The checkpoint: the disc, the titles done, the backup a backup-then-MKV job rips from and the read errors
        /// of runs no title owns (a backup's: its MKV files inherit them). The staging folder is synced first, so what the
        /// checkpoint names is there after a power loss.</summary>
        Task Save(string kind)
        {
            _fs.SyncDirectory(_staging);
            return _c.Checkpoint.Save(JsonValue.Of(("kind", JsonValue.Of(kind)), ("fingerprint", JsonValue.Of(_probe.Fingerprint)),
                ("titles", JsonValue.Of(_titles.Select(AcquiredTitle.Encode))), ("backup", JsonValue.Of(_backup)),
                ("readErrors", JsonValue.Of(_readErrors.Select(e => JsonValue.Of(e))))));
        }

        /// <summary>The plan's profile, with Plan's one-pass minimum length when given.</summary>
        MakemkvInvocation Invocation(int? minLength, string? selection)
        {
            var profile = _plan.Profile;
            if (minLength is { } m && profile is JsonValue.Object o)
            {
                var makemkv = (o["makemkv"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
                    .Where(kv => kv.Key != "minLengthSeconds").Append(new("minLengthSeconds", JsonValue.Of(m))).ToList();
                profile = new JsonValue.Object(o.Members.Where(kv => kv.Key != "makemkv").Append(new("makemkv", new JsonValue.Object(makemkv))).ToList());
            }
            return _c.MakemkvSetup.Invocation(_c.Config, new EffectiveProfile(profile, new List<string>(), new List<ResolveTrace>()), selection,
                _c.JobDirectory);
        }

        JsonValue? Profile(string section, string key) => _plan.Profile[section]?[key];

        MakemkvSink Sink(BroMessage operation, double from, double width) => new(new Portion(_c.Progress, from, width), _c.Log, operation);

        void LogRun(MakemkvRun run)
        {
            foreach (var problem in new[] { run.IsolationWarning, run.TranscriptProblem, run.SettingsProblem })
                if (problem != null) _c.Log.Log(problem with { Severity = Severity.Warning });
        }

        StepReport Report(BroError? error)
        {
            var titles = _titles.OrderBy(t => _plan.Titles.Select(p => p.Index).ToList().IndexOf(t.Index)).ToList();
            var output = new AcquireOutput(titles, _others.ToList(), _backup, _discInfo, _readErrors.ToList());
            return new(AcquireOutput.Encode(output), ReadErrors: AcquireOutput.ReadErrorCount(output), Notice: error != null ? _notice : null, Error: error);
        }

        StepReport Done() => Report(null);

        StepReport Failed(BroMessage message) => Report(message.ToError());

        StepReport Cancelled() => Report(new BroError(MessageCode.Wire(MessageCode.JobCancelled)));

        static BroFailure NoInformation() => new(new BroMessage(MessageCode.DiscNoInformation, Severity.Error).ToError());
    }

    /// <summary>A run's progress as a part of the step's.</summary>
    sealed class Portion : IProgressReporter
    {
        readonly IProgressReporter _inner;
        readonly double _from;
        readonly double _width;

        public Portion(IProgressReporter inner, double from, double width)
        {
            _inner = inner;
            _from = from;
            _width = width;
        }

        public void Report(double fraction, BroMessage operation, string? current = null) =>
            _inner.Report(_from + _width * Math.Clamp(fraction, 0, 1), operation, current);
    }
}
