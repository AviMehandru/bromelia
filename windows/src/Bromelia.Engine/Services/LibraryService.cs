using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Engine;

/// <summary>
/// The libraries of the configuration (plan §11.1, §22). A library is online while its folder holds the
/// .bromelia-library marker it was attached with (I2): the markers are checked at start, every 30 s and on
/// <see cref="Check"/> (mount events), each on its own worker; an offline library is blocked in the scheduler and raised
/// as a problem. A check that hasn't come back by the next tick (a share that froze, review L3-9) counts as offline
/// and isn't started again until it comes back. As the <see cref="ILibraryPort"/> of the steps it gives staging folders
/// and runs the commit protocol (§22.3 steps 4–6) and its roll forward (§15.8) on workers, one at a time per unit: a
/// commit waits while a roll forward of its unit runs (recovery's may outlast its 30 s at startup on a slow share). State
/// is touched only on the executor; the steps read a snapshot. shared/fixtures/engine/library-service.cases.json.
/// </summary>
public sealed class LibraryService : ILibraryPort
{
    const int FileMode = 0x1A4; // 0644
    const string Staging = ".bromelia";
    const string QuarantineFolder = "_Quarantine";
    const string IncompleteNote = "INCOMPLETE.txt";
    /// <summary>The media server layout's folder for backups (Layouts); servers are kept out of it by the ignore
    /// files (owner, 2026-10-22).</summary>
    const string BackupFolder = "Backup";
    /// <summary>.plexignore for Plex, .ignore for Jellyfin and Emby, as today's apps wrote them.</summary>
    static readonly (string Name, string Text)[] IgnoreFiles = { (".plexignore", "*\n"), (".ignore", "") };
    static readonly Duration CheckEvery = new(30);
    static readonly MessageCode[] ReasonCodes = { MessageCode.LibraryOffline, MessageCode.LibraryNotAttached, MessageCode.LibraryMarkerMismatch };

    readonly EngineExecutor _executor;
    readonly EventBus _bus;
    readonly ProblemService _problems;
    readonly Scheduler _scheduler;
    readonly IConfigSource _config;
    readonly EngineClock _clock;
    readonly IFileSystem _fs;
    readonly ILibraryRepository _libraries;
    readonly IUnitRepository _units;
    readonly Action<string>? _crash;

    sealed class Entry
    {
        public required string Id;
        public required string Name;
        public required string Path;
        public bool Known;
        public bool Online;
        public BroMessage? Reason;
        public long? Free;
        public Instant? Seen;
        public bool InFlight;
    }

    /// <summary>What a marker check found.</summary>
    sealed record Found(BroMessage? Reason, long? Free, Instant? Seen);

    /// <summary>Checks applied together, in configuration order, when all have come back or at the next tick.</summary>
    sealed class Round
    {
        public readonly HashSet<Entry> Pending = new();
        public readonly Dictionary<Entry, Found> Results = new();
        public readonly List<TaskCompletionSource> Waiters = new();
    }

    readonly List<Entry> _entries = new();
    Round? _round;
    EngineTimer? _timer;
    Subscription? _subscription;

    readonly object _lock = new();
    Dictionary<string, (string Name, string Path, BroMessage? Reason)> _snapshot = new();
    /// <summary>The units a commit or roll forward is writing; each completes when that one is done.</summary>
    readonly Dictionary<Id, TaskCompletionSource> _committing = new();
    int _waiting;

    /// <param name="crash">Called at each crash point of the commit (commit.afterIntent, commit.afterMove:&lt;n&gt;,
    /// commit.beforeDone); the test root throws there to stop the commit as a killed engine would. Null in a real
    /// engine.</param>
    public LibraryService(EngineExecutor executor, EventBus bus, ProblemService problems, Scheduler scheduler, IConfigSource config,
        EngineClock clock, IFileSystem fs, ILibraryRepository libraries, IUnitRepository units, Action<string>? crash = null)
    {
        _executor = executor;
        _bus = bus;
        _problems = problems;
        _scheduler = scheduler;
        _config = config;
        _clock = clock;
        _fs = fs;
        _libraries = libraries;
        _units = units;
        _crash = crash;
    }

    /// <summary>Reads when each library was last seen, checks every library, then every 30 s, and follows
    /// configChanged; completes when the first round is applied.</summary>
    public async Task Start()
    {
        var rows = await _executor.Worker(() => _libraries.All());
        Sync();
        foreach (var e in _entries)
            if (rows.FirstOrDefault(r => r.Id == e.Id && r.Path == e.Path) is { } row) e.Seen = row.LastSeenAt;
        _subscription = _bus.Subscribe(new[] { EngineEventType.ConfigChanged }, _ => OnConfigChanged());
        _timer = _clock.Timer(new TimerSchedule.Every(CheckEvery), Tick);
        await StartRound();
    }

    public void Stop()
    {
        _timer?.Cancel();
        _timer = null;
        _subscription?.Cancel();
        _subscription = null;
    }

    /// <summary>In configuration order (on the executor).</summary>
    public IReadOnlyList<LibraryDto> Libraries() => _entries.Select(Dto).ToList();

    /// <summary>Checks every marker now (mount events); while a round is still out, waits for it instead.</summary>
    public Task Check() => StartRound();

    /// <summary>Writes the marker into an existing folder (never creates it), stores the library and brings it online:
    /// library.pathMissing{path}, library.alreadyAttached{path, library}.</summary>
    public async Task<LibraryDto> Attach(string libraryId, string path, string name)
    {
        var display = name.Length > 0 ? name : LastPart(path);
        var now = _clock.Now();
        var marker = LibraryMarker.Make(libraryId, path, display, now);
        var free = await _executor.Worker(async () =>
        {
            if (!_fs.Exists(path) || !_fs.Stat(path).IsDirectory)
                throw new BroFailure(Message(MessageCode.LibraryPathMissing, ("path", JsonValue.Of(path))).ToError());
            var markerPath = path + "/" + LibraryMarker.FileName;
            if (_fs.Exists(markerPath))
            {
                var found = Decode(markerPath);
                throw new BroFailure(Message(MessageCode.LibraryAlreadyAttached, ("path", JsonValue.Of(path)),
                    ("library", JsonValue.Of(found is { Name.Length: > 0 } ? found.Name : libraryId))).ToError());
            }
            _fs.WriteAtomically(markerPath, LibraryMarker.Encode(marker), FileMode);
            await _libraries.Upsert(new LibraryRecord(libraryId, display, path, marker.Id, now, true, now)).ConfigureAwait(false);
            return FreeBytes(path);
        });
        var e = Find(libraryId);
        if (e == null)
        {
            e = new Entry { Id = libraryId, Name = display, Path = path };
            _entries.Add(e);
        }
        e.Path = path;
        Apply(e, new Found(null, free, now));
        return Dto(e);
    }

    // ---- the port (any thread) ----------------------------------------------------------------------------------

    public LibraryHandle Require(string libraryId) => new(libraryId, Online(libraryId).Path);

    /// <summary>The library's name and root, from the snapshot: library.notFound, or why it is offline.</summary>
    (string Id, string Name, string Path) Online(string libraryId)
    {
        lock (_lock)
        {
            if (!_snapshot.TryGetValue(libraryId, out var s)) throw new BroFailure(Message(MessageCode.LibraryNotFound).ToError());
            if (s.Reason != null) throw new BroFailure(s.Reason.ToError());
            return (libraryId, s.Name, s.Path);
        }
    }

    public string StagingDirectory(string libraryId, Id jobId)
    {
        var dir = Require(libraryId).Path;
        foreach (var part in new[] { Staging, "staging", jobId.Value })
        {
            dir += "/" + part;
            if (!_fs.Exists(dir)) _fs.CreateDirectory(dir, parentsMustExist: true);
        }
        return dir;
    }

    public Task<CommitResult> Commit(UnitRecord unit, CommitIntent intent, IReadOnlyList<UnitFile> files, CancellationToken cancel) =>
        Run(unit, intent, files, null, cancel);

    public Task<CommitResult> Quarantine(UnitRecord unit, CommitIntent intent, IReadOnlyList<UnitFile> files, string tag, CancellationToken cancel) =>
        Run(unit, intent, files, tag, cancel);

    /// <summary>Finishes an interrupted commit (§15.8): once no other commit of the unit runs, the marker checked, then
    /// what is left of the unit's open intent (read again: <paramref name="intent"/> may be stale) moved (a file already
    /// at its destination counts when its SHA-256 is the item's), SHA256SUMS merged, the folders synced and the unit
    /// committed, its files read from the destination's SHA256SUMS. Null when the intent is no longer open (the job's
    /// Commit finished it meanwhile). Any thread but the executor.</summary>
    public Task<CommitResult?> RollForward(CommitIntent intent) => _executor.Worker(async () =>
    {
        var unit = await _units.Get(intent.UnitId).ConfigureAwait(false)
            ?? throw new BroFailure(Message(MessageCode.UnitNotFound).ToError());
        var lib = await Configured(unit.LibraryId).ConfigureAwait(false);
        await Claim(intent.UnitId, null).ConfigureAwait(false);
        try
        {
            await CheckMarker(lib).ConfigureAwait(false);
            var open = await OpenIntent(intent.UnitId).ConfigureAwait(false);
            return open == null ? null : await Complete(lib.Path, open, null).ConfigureAwait(false);
        }
        finally
        {
            Unclaim(intent.UnitId);
        }
    });

    /// <summary>Commits and roll forwards waiting for another one of their unit to finish (a test or scenario runner
    /// counts them as parked when it settles). Any thread.</summary>
    public int Waiting
    {
        get
        {
            lock (_lock) return _waiting;
        }
    }

    /// <summary>The folders in the library's .bromelia/staging (one per job, named by its id), in ordinal order,
    /// once the marker checks out (from the configuration itself, so it works before <see cref="Start"/>). Any thread
    /// but the executor.</summary>
    public Task<IReadOnlyList<string>> StagingFolders(string libraryId) => _executor.Worker(async () =>
    {
        var lib = await Configured(libraryId).ConfigureAwait(false);
        await CheckMarker(lib).ConfigureAwait(false);
        var staging = lib.Path + "/" + Staging + "/staging";
        if (!IsFolder(staging)) return (IReadOnlyList<string>)Array.Empty<string>();
        return _fs.List(staging).Where(e => e.IsDirectory).Select(e => e.Name).OrderBy(n => n, StringComparer.Ordinal).ToList();
    });

    /// <summary>A staging folder no job will continue goes to &lt;library&gt;/_Quarantine/&lt;ConflictNamer('&lt;name&gt;
    /// [INCOMPLETE]')&gt; with <paramref name="note"/> as INCOMPLETE.txt and <paramref name="logs"/> (the job's log copies,
    /// by name; owner, 2026-10-25) next to it, each written atomically before the move, so the folder arrives marked (I1);
    /// never reused or deleted (review L3-3, P1-3); one that holds no file is removed instead (null), and nothing is
    /// written into it. The folders are synced. Any thread but the executor.</summary>
    public Task<string?> SetAside(string libraryId, string folder, string name, byte[] note, IReadOnlyList<(string Name, byte[] Bytes)> logs) =>
        _executor.Worker(async () =>
    {
        var lib = await Configured(libraryId).ConfigureAwait(false);
        await CheckMarker(lib).ConfigureAwait(false);
        var staging = lib.Path + "/" + Staging + "/staging/" + folder;
        if (!IsFolder(staging)) throw NotFound(staging);
        if (!Walk(staging).Any())
        {
            RemoveEmpty(staging);
            return (string?)null;
        }
        foreach (var (logName, bytes) in logs) _fs.WriteAtomically(staging + "/" + logName, bytes, FileMode);
        _fs.WriteAtomically(staging + "/" + IncompleteNote, note, FileMode);
        var quarantine = lib.Path + "/" + QuarantineFolder;
        EnsureFolders(lib.Path, quarantine);
        var dest = quarantine + "/" + ConflictNamer.Next(name + " [INCOMPLETE]", _fs.List(quarantine).Select(e => e.Name).ToList(), isFolder: true);
        _fs.Rename(staging, dest);
        _fs.SyncDirectory(quarantine);
        _fs.SyncDirectory(Parent(staging));
        return dest;
    });

    // ---- marker checks (executor) ---------------------------------------------------------------------------------

    void Sync()
    {
        var wanted = Config.Libraries(_config.Current());
        foreach (var gone in _entries.Where(e => wanted.All(w => w.Id != e.Id)).ToList())
        {
            _entries.Remove(gone);
            _scheduler.SetLibraryOffline(gone.Id, null);
            foreach (var code in ReasonCodes) _problems.Clear(code, Scope(gone.Id));
        }
        var ordered = new List<Entry>();
        foreach (var w in wanted)
        {
            var e = Find(w.Id);
            if (e == null || e.Path != w.Path) e = new Entry { Id = w.Id, Name = w.Name.Length > 0 ? w.Name : w.Id, Path = w.Path };
            else e.Name = w.Name.Length > 0 ? w.Name : w.Id;
            ordered.Add(e);
        }
        _round?.Pending.RemoveWhere(e => !ordered.Contains(e));
        _entries.Clear();
        _entries.AddRange(ordered);
        UpdateSnapshot();
        if (_round is { Pending.Count: 0 } open) Close(open);
    }

    void OnConfigChanged()
    {
        Sync();
        _ = StartRound();
    }

    void Tick()
    {
        if (_round != null) Close(_round);
        _ = StartRound();
    }

    /// <summary>Checks every library that has no check out, in the open round (a new one when there is none);
    /// completes when that round is applied.</summary>
    Task StartRound()
    {
        var done = new TaskCompletionSource();
        var round = _round ??= new Round();
        round.Waiters.Add(done);
        foreach (var e in _entries.Where(e => !e.InFlight))
        {
            e.InFlight = true;
            round.Pending.Add(e);
            Launch(e, round);
        }
        if (round.Pending.Count == 0) Close(round);
        return done.Task;
    }

    /// <summary>async void: a failure that isn't a BroFailure is a bug, and reaches the executor's handler.</summary>
    async void Launch(Entry e, Round round)
    {
        var found = await _executor.Worker(() => Inspect(e.Id, e.Name, e.Path));
        e.InFlight = false;
        var current = _entries.Contains(e);
        if (_round == round && round.Pending.Remove(e))
        {
            if (current) round.Results[e] = found;
            if (round.Pending.Count == 0) Close(round);
        }
        else if (current)
        {
            Apply(e, found);
        }
    }

    /// <summary>Applies what came back in configuration order; libraries still out are offline.</summary>
    void Close(Round round)
    {
        _round = null;
        foreach (var e in _entries.ToList())
        {
            if (round.Results.TryGetValue(e, out var found)) Apply(e, found);
            else if (round.Pending.Contains(e))
            {
                var stuck = new Found(Offline(e.Name, e.Path), null, e.Seen);
                var id = e.Id;
                var now = _clock.Now();
                _ = _executor.Worker(async () =>
                {
                    try { await _libraries.SetSeen(id, false, now).ConfigureAwait(false); }
                    catch (BroFailure) { }
                    return true;
                });
                Apply(e, stuck);
            }
        }
        foreach (var w in round.Waiters) w.SetResult();
    }

    void Apply(Entry e, Found found)
    {
        var changed = !e.Known || e.Online != (found.Reason == null) || !Equals(e.Reason, found.Reason);
        e.Known = true;
        e.Online = found.Reason == null;
        e.Reason = found.Reason;
        e.Free = found.Free;
        if (found.Seen != null) e.Seen = found.Seen;
        UpdateSnapshot();
        if (!changed) return;
        _scheduler.SetLibraryOffline(e.Id, found.Reason?.ToError());
        _bus.Publish(EngineEventType.LibraryChanged, JsonValue.Of(("library", LibraryDto.Json(Dto(e)))));
        foreach (var code in ReasonCodes.Where(c => c != found.Reason?.Code)) _problems.Clear(code, Scope(e.Id));
        if (found.Reason is { } reason)
            _problems.Raise(reason, Scope(e.Id),
                new[] { reason.Code == MessageCode.LibraryNotAttached ? ProblemAction.AttachLibrary : ProblemAction.OpenSettings });
    }

    void UpdateSnapshot()
    {
        var snapshot = _entries.ToDictionary(e => e.Id, e => (e.Name, e.Path, e.Known ? e.Reason : Offline(e.Name, e.Path)));
        lock (_lock) _snapshot = snapshot;
    }

    Entry? Find(string id) => _entries.FirstOrDefault(e => e.Id == id);

    static LibraryDto Dto(Entry e) => new(e.Id, e.Name, e.Path, e.Known && e.Online, e.Known ? e.Reason : null, e.Online ? e.Free : null, e.Seen);

    static ProblemScope Scope(string id) => new(ProblemScopeKind.Library, id);

    // ---- marker checks (workers) ----------------------------------------------------------------------------------

    /// <summary>The marker check of one library; stores what it found.</summary>
    async Task<Found> Inspect(string id, string name, string path)
    {
        LibraryRecord? row = null;
        BroMessage? reason;
        try
        {
            row = await _libraries.Get(id).ConfigureAwait(false);
            reason = await MarkerReason(id, name, path, row).ConfigureAwait(false);
        }
        catch (BroFailure)
        {
            reason = Offline(name, path);
        }
        var now = _clock.Now();
        try { await _libraries.SetSeen(id, reason == null, now).ConfigureAwait(false); }
        catch (BroFailure) { }
        return reason == null ? new Found(null, FreeBytes(path), now) : new Found(reason, null, row?.LastSeenAt);
    }

    /// <summary>Null when the folder holds the stored marker (adopting a marker that isn't stored yet); otherwise why
    /// the library is offline.</summary>
    async Task<BroMessage?> MarkerReason(string id, string name, string path, LibraryRecord? row)
    {
        if (!_fs.Exists(path)) return Offline(name, path);
        var markerPath = path + "/" + LibraryMarker.FileName;
        if (!_fs.Exists(markerPath)) return row == null ? Message(MessageCode.LibraryNotAttached, Severity.Warning, ("path", JsonValue.Of(path))) : Offline(name, path);
        var marker = Decode(markerPath);
        if (marker == null) return Offline(name, path);
        if (row == null || row.Path != path)
        {
            var now = _clock.Now();
            await _libraries.Upsert(new LibraryRecord(id, name, path, marker.Id, marker.Created ?? now, true, now)).ConfigureAwait(false);
            return null;
        }
        if (row.MarkerId != marker.Id)
            return Message(MessageCode.LibraryMarkerMismatch, ("path", JsonValue.Of(path)), ("found", JsonValue.Of(marker.Id)),
                ("expected", JsonValue.Of(row.MarkerId)));
        return null;
    }

    async Task CheckMarker((string Id, string Name, string Path) lib)
    {
        BroMessage? reason;
        try
        {
            reason = await MarkerReason(lib.Id, lib.Name, lib.Path, await _libraries.Get(lib.Id).ConfigureAwait(false)).ConfigureAwait(false);
        }
        catch (BroFailure)
        {
            reason = Offline(lib.Name, lib.Path);
        }
        if (reason != null) throw new BroFailure(reason.ToError());
    }

    LibraryMarker? Decode(string markerPath)
    {
        try { return LibraryMarker.Decode(_fs.Read(markerPath)); }
        catch (BroFailure) { return null; }
    }

    long? FreeBytes(string path)
    {
        try { return _fs.Volume(path).FreeBytes; }
        catch (BroFailure) { return null; }
    }

    /// <summary>The configured library (from the configuration itself, so a roll forward works before
    /// <see cref="Start"/>).</summary>
    Task<(string Id, string Name, string Path)> Configured(string libraryId)
    {
        var done = new TaskCompletionSource<(string, string, string)>();
        _executor.Post(() =>
        {
            var w = Config.Libraries(_config.Current()).FirstOrDefault(l => l.Id == libraryId);
            if (w == null) done.SetException(new BroFailure(Message(MessageCode.LibraryNotFound).ToError()));
            else done.SetResult((w.Id, w.Name.Length > 0 ? w.Name : w.Id, w.Path));
        });
        return done.Task;
    }

    // ---- the commit (workers) -------------------------------------------------------------------------------------

    Task<CommitResult> Run(UnitRecord unit, CommitIntent intent, IReadOnlyList<UnitFile> files, string? tag, CancellationToken cancel) =>
        _executor.Worker(async () =>
    {
        var lib = Online(unit.LibraryId);
        if (tag == null && !Inside(lib.Path, intent.Destination)) throw new BroFailure(Message(MessageCode.LibraryNotFound).ToError());
        await Claim(unit.Id, cancel).ConfigureAwait(false);
        try
        {
            await CheckMarker(lib).ConfigureAwait(false);
            var open = await OpenIntent(unit.Id).ConfigureAwait(false);
            if (open != null) return await Complete(lib.Path, open, files).ConfigureAwait(false);
            // Committed already (a roll forward finished it, or the engine stopped before the step's result was stored).
            // Staging then holds none of its items; one that does was made again (a job retried after its commit) and
            // is never passed off as the unit in the library.
            if (await _units.Get(unit.Id).ConfigureAwait(false) is { State: UnitState.Committed or UnitState.Quarantined } done)
            {
                if (intent.Items.Any(i => _fs.Exists(intent.Staging + "/" + i.FromPath))) throw AlreadyExists(lib.Path + "/" + done.Path);
                RemoveEmpty(intent.Staging);
                return new CommitResult(unit.Id, lib.Path + "/" + done.Path, files, done.State == UnitState.Committed && intent.Merge);
            }
            if (tag != null)
            {
                var folder = lib.Path + "/" + QuarantineFolder;
                EnsureFolders(lib.Path, folder);
                var name = ConflictNamer.Next(LastPart(intent.Destination) + " [" + tag + "]", _fs.List(folder).Select(e => e.Name).ToList(), isFolder: true);
                intent = intent with { Destination = folder + "/" + name, Merge = false, Quarantine = true };
                unit = unit with { Path = QuarantineFolder + "/" + name };
            }
            Precheck(intent);
            await _units.BeginCommit(unit with { State = UnitState.Committing }, intent).ConfigureAwait(false);
            _crash?.Invoke("commit.afterIntent");
            return await Complete(lib.Path, intent, files).ConfigureAwait(false);
        }
        finally
        {
            Unclaim(unit.Id);
        }
    });

    /// <summary>Nothing is written unless every item is in staging with SHA256SUMS, and nothing at the destination
    /// is in the way (I5).</summary>
    void Precheck(CommitIntent intent)
    {
        if (!IsFolder(intent.Staging)) throw NotFound(intent.Staging);
        if (!_fs.Exists(intent.Staging + "/" + Sha256Sums.FileName)) throw NotFound(intent.Staging + "/" + Sha256Sums.FileName);
        foreach (var item in intent.Items)
            if (!_fs.Exists(intent.Staging + "/" + item.FromPath)) throw NotFound(intent.Staging + "/" + item.FromPath);
        if (!intent.Merge)
        {
            if (_fs.Exists(intent.Destination))
                throw new BroFailure(Message(MessageCode.LibraryFolderExists, ("path", JsonValue.Of(intent.Destination))).ToError());
            return;
        }
        foreach (var item in intent.Items)
        {
            var targets = item.IsDir
                ? Walk(intent.Staging + "/" + item.FromPath).Select(f => intent.Destination + "/" + item.ToPath + "/" + f)
                : new[] { intent.Destination + "/" + item.ToPath };
            foreach (var t in targets)
                if (_fs.Exists(t) && !_fs.Stat(t).IsDirectory) throw AlreadyExists(t);
        }
    }

    /// <summary>Steps 5 and 6 from wherever the intent is: the move, SHA256SUMS, the syncs, then the unit committed.</summary>
    async Task<CommitResult> Complete(string root, CommitIntent intent, IReadOnlyList<UnitFile>? files)
    {
        var moved = intent.Items.Count(i => i.Moved);
        var pending = intent.Items.Where(i => !i.Moved).OrderBy(i => i.Seq).ToList();
        var whole = !intent.Merge && pending.Count > 0 && IsFolder(intent.Staging) && !_fs.Exists(intent.Destination);
        if (whole)
        {
            EnsureFolders(root, Parent(intent.Destination));
            _fs.Rename(intent.Staging, intent.Destination);
        }
        foreach (var item in pending)
        {
            if (!whole) Move(root, intent, item);
            await _units.MarkMoved(intent.UnitId, item.Seq).ConfigureAwait(false);
            moved++;
            _crash?.Invoke("commit.afterMove:" + moved);
        }
        var stagingSums = intent.Staging + "/" + Sha256Sums.FileName;
        var sums = intent.Destination + "/" + Sha256Sums.FileName;
        if (_fs.Exists(stagingSums))
        {
            EnsureFolders(root, intent.Destination);
            if (intent.Merge || _fs.Exists(sums))
            {
                var existing = _fs.Exists(sums) ? Text(_fs.Read(sums)) : "";
                _fs.WriteAtomically(sums, Encoding.UTF8.GetBytes(Sha256Sums.Merge(existing, Sha256Sums.Parse(Text(_fs.Read(stagingSums))))), FileMode);
                _fs.Remove(stagingSums);
            }
            else
            {
                _fs.Rename(stagingSums, sums);
            }
        }
        if (intent.Merge) Ignored(intent);
        RemoveEmpty(intent.Staging);
        var unitFiles = files ?? FromSums(intent);
        foreach (var folder in Synced(root, intent, unitFiles)) _fs.SyncDirectory(folder);
        _crash?.Invoke("commit.beforeDone");
        var record = await _units.Get(intent.UnitId).ConfigureAwait(false) ?? throw new BroFailure(Message(MessageCode.UnitNotFound).ToError());
        record = record with { State = intent.Quarantine ? UnitState.Quarantined : UnitState.Committed, CommittedAt = _clock.Now() };
        await _units.FinishCommit(intent, record, unitFiles).ConfigureAwait(false);
        return new CommitResult(intent.UnitId, intent.Destination, unitFiles, intent.Merge);
    }

    /// <summary>One item: a file renamed, a folder merged; one already at its destination is recognised by its hash,
    /// never replaced.</summary>
    void Move(string root, CommitIntent intent, CommitItem item)
    {
        var from = intent.Staging + "/" + item.FromPath;
        var to = intent.Destination + "/" + item.ToPath;
        bool there = _fs.Exists(from), arrived = _fs.Exists(to);
        if (item.IsDir)
        {
            if (there)
            {
                EnsureFolders(root, Parent(to));
                _fs.MoveMerging(from, to, MovePolicy.NeverReplace);
            }
            else if (!arrived) throw NotFound(from);
            return;
        }
        if (there && !arrived)
        {
            EnsureFolders(root, Parent(to));
            _fs.Rename(from, to);
        }
        else if (!there && arrived)
        {
            if (item.Sha256 != null && Hash(to) != item.Sha256) throw AlreadyExists(to);
        }
        else if (there) throw AlreadyExists(to);
        else throw NotFound(from);
    }

    /// <summary>A media server's Backup/ folder the unit was merged into gets .plexignore and .ignore when they are missing
    /// (owner, 2026-10-22): outside the unit (not in SHA256SUMS), never overwritten, each written atomically. Told from
    /// the intent alone (an item under Backup/ of a shared folder), so a roll forward writes them too.</summary>
    void Ignored(CommitIntent intent)
    {
        if (!intent.Items.Any(i => i.ToPath.StartsWith(BackupFolder + "/", StringComparison.Ordinal))) return;
        var folder = intent.Destination + "/" + BackupFolder;
        if (!IsFolder(folder)) return;
        foreach (var (name, text) in IgnoreFiles)
            if (!_fs.Exists(folder + "/" + name)) _fs.WriteAtomically(folder + "/" + name, Encoding.UTF8.GetBytes(text), FileMode);
    }

    /// <summary>The unit's lines of the destination's SHA256SUMS (a roll forward has no file list of its own).</summary>
    IReadOnlyList<UnitFile> FromSums(CommitIntent intent)
    {
        var sums = Sha256Sums.Parse(Text(_fs.Read(intent.Destination + "/" + Sha256Sums.FileName)));
        return sums.Where(s => intent.Items.Any(i => i.IsDir ? s.Path.StartsWith(i.ToPath + "/", StringComparison.Ordinal) : s.Path == i.ToPath))
            .Select(s => new UnitFile(s.Path, _fs.Stat(intent.Destination + "/" + s.Path).Size, s.Sha256, "title")).ToList();
    }

    /// <summary>Every folder from the library's root down to each committed file, and the staging folder's parent.</summary>
    static IReadOnlyList<string> Synced(string root, CommitIntent intent, IReadOnlyList<UnitFile> files)
    {
        var folders = new SortedSet<string>(StringComparer.Ordinal) { Parent(intent.Staging) };
        foreach (var f in files)
        {
            var folder = root;
            folders.Add(folder);
            var parts = (intent.Destination + "/" + f.Path)[(root.Length + 1)..].Split('/');
            foreach (var part in parts.Take(parts.Length - 1))
            {
                folder += "/" + part;
                folders.Add(folder);
            }
        }
        return folders.ToList();
    }

    /// <summary>Makes the folders from below the library's root down to <paramref name="folder"/>; never the root.</summary>
    void EnsureFolders(string root, string folder)
    {
        if (folder == root) return;
        if (!Inside(root, folder)) throw new BroFailure(Message(MessageCode.LibraryNotFound).ToError());
        var current = root;
        foreach (var part in folder[(root.Length + 1)..].Split('/'))
        {
            current += "/" + part;
            if (!_fs.Exists(current)) _fs.CreateDirectory(current, parentsMustExist: true);
        }
    }

    /// <summary>Removes the staging folder's empty folders, and the folder itself once empty; anything else stays.</summary>
    void RemoveEmpty(string folder)
    {
        if (!IsFolder(folder)) return;
        foreach (var e in _fs.List(folder).Where(e => e.IsDirectory)) RemoveEmpty(folder + "/" + e.Name);
        if (_fs.List(folder).Count == 0) _fs.Remove(folder);
    }

    IEnumerable<string> Walk(string folder)
    {
        foreach (var e in _fs.List(folder).OrderBy(e => e.Name, StringComparer.Ordinal))
            if (e.IsDirectory) foreach (var f in Walk(folder + "/" + e.Name)) yield return e.Name + "/" + f;
            else yield return e.Name;
    }

    string Hash(string path)
    {
        var stream = _fs.OpenForReading(path, bypassCache: true);
        try
        {
            var hasher = new Sha256Hasher();
            for (var chunk = stream.Read(1 << 20); chunk.Length > 0; chunk = stream.Read(1 << 20)) hasher.Update(chunk);
            return hasher.Finish();
        }
        finally
        {
            stream.Close();
        }
    }

    bool IsFolder(string path) => _fs.Exists(path) && _fs.Stat(path).IsDirectory;

    async Task<CommitIntent?> OpenIntent(Id unitId) => (await _units.OpenIntents().ConfigureAwait(false)).FirstOrDefault(i => i.UnitId == unitId);

    /// <summary>Holds the unit for one commit or roll forward, first waiting while another one has it; job.cancelled when
    /// <paramref name="cancel"/> fires while it waits (once held, a commit runs to its end or its intent stays).</summary>
    async Task Claim(Id unitId, CancellationToken? cancel)
    {
        while (true)
        {
            Task busy;
            lock (_lock)
            {
                if (cancel?.IsCancelled == true) throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.JobCancelled)));
                if (!_committing.TryGetValue(unitId, out var holder))
                {
                    _committing[unitId] = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
                    return;
                }
                busy = holder.Task;
                _waiting++;
            }
            var cancelled = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            using (cancel?.OnCancel(() => cancelled.TrySetResult()))
                await Task.WhenAny(busy, cancelled.Task).ConfigureAwait(false);
            lock (_lock) _waiting--;
        }
    }

    void Unclaim(Id unitId)
    {
        TaskCompletionSource? holder;
        lock (_lock) _committing.Remove(unitId, out holder);
        holder?.SetResult();
    }

    /// <summary>Strictly below the root, and not in its .bromelia folder.</summary>
    static bool Inside(string root, string path) =>
        path.StartsWith(root + "/", StringComparison.Ordinal) && path != root + "/" + Staging && !path.StartsWith(root + "/" + Staging + "/", StringComparison.Ordinal);

    static string Parent(string path) => path[..path.LastIndexOf('/')];

    static string LastPart(string path) => path.TrimEnd('/')[(path.TrimEnd('/').LastIndexOf('/') + 1)..];

    static string Text(byte[] bytes) => Encoding.UTF8.GetString(bytes);

    static BroMessage Offline(string name, string path) =>
        Message(MessageCode.LibraryOffline, Severity.Warning, ("library", JsonValue.Of(name)), ("path", JsonValue.Of(path)));

    static BroMessage Message(MessageCode code, params (string, JsonValue)[] parameters) => new(code, Severity.Error, parameters);

    static BroMessage Message(MessageCode code, Severity severity, params (string, JsonValue)[] parameters) => new(code, severity, parameters);

    static BroFailure NotFound(string path) => new(Message(MessageCode.FsNotFound, ("path", JsonValue.Of(path))).ToError());

    static BroFailure AlreadyExists(string path) => new(Message(MessageCode.FsAlreadyExists, ("path", JsonValue.Of(path))).ToError());
}
