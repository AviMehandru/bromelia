using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Engine;

/// <summary>
/// The optical drives (plan §11.1, §15.2), on the executor. A scan (MakeMKV's drive list and the OS's, on a worker,
/// one at a time) runs at start, every automation.pollIntervalSeconds, on the OS's device events and after a tray
/// command; while an acquisition lease is held only asked-for scans run, unless automation.pollWhileRipping. DriveJoin
/// pairs the two lists; a drive's id comes from MakeMKV's identification. Each drive counts its discs: a disc that
/// arrives gets the next generation and mediumArrived (not the discs already in at start, which never start a rip),
/// one that leaves mediumRemoved. A drive a job holds keeps what it had (MakeMKV can't see a drive in use), and a
/// drive left out of a scan keeps it until the OS says it's gone. Configured drives that aren't connected stay listed
/// (connected false); others are removed. Close tray checks, in the first scan 4 s later, that the tray closed
/// (drive.trayStillOpen: a drive without a tray motor). As the <see cref="IDriveLookup"/> of the steps it gives a
/// snapshot, from any thread. shared/fixtures/engine/drive-service.cases.json.
/// </summary>
public sealed class DriveService : IDriveLookup
{
    static readonly Duration TrayCheckAfter = new(4);
    static readonly Duration ScanAfterTray = new(5);
    static readonly Duration ScanAfterEject = new(3);

    readonly EngineExecutor _executor;
    readonly EventBus _bus;
    readonly ProblemService _problems;
    readonly IConfigSource _config;
    readonly EngineClock _clock;
    readonly IDeviceMonitor _monitor;
    readonly IDriveControl _control;
    readonly IMakemkv _makemkv;
    readonly IDriveRepository _repository;

    sealed class Drive
    {
        public required string Id;
        /// <summary>A configured entry no connected drive matches (listed only).</summary>
        public string? EntryId;
        public MakemkvDrive? Makemkv;
        public string? OsDevice;
        public bool Connected;
        public DriveStatus State = DriveStatus.Unavailable;
        public MediumDto? Medium;
        public DriveDto? Shown;
    }

    /// <summary>What a scan's worker found.</summary>
    sealed record Found(IReadOnlyList<MakemkvDrive> Makemkv, IReadOnlyList<OsDrive> Os, Dictionary<string, DiscContent> Contents, BroError? Failure);

    readonly List<Drive> _drives = new();
    readonly Dictionary<string, long> _generations = new();
    readonly Dictionary<string, DriveRecord> _stored = new();
    readonly Dictionary<string, Instant> _trayAsked = new();
    LeaseSnapshot _leases = LeaseSnapshot.Empty;
    bool _started, _firstDone, _scanning, _again, _deferred;
    List<TaskCompletionSource> _waiting = new(), _next = new();
    EngineTimer? _poll;
    long _pollSeconds = -1;
    readonly List<Subscription> _subscriptions = new();

    readonly object _lock = new();
    Dictionary<string, DriveLocation> _snapshot = new();

    public DriveService(EngineExecutor executor, EventBus bus, ProblemService problems, IConfigSource config, EngineClock clock,
        IDeviceMonitor monitor, IDriveControl control, IMakemkv makemkv, IDriveRepository repository)
    {
        _executor = executor;
        _bus = bus;
        _problems = problems;
        _config = config;
        _clock = clock;
        _monitor = monitor;
        _control = control;
        _makemkv = makemkv;
        _repository = repository;
    }

    /// <summary>Reads the stored drives, follows leases, the configuration and the OS's events, and scans; completes
    /// when the first scan is applied.</summary>
    public async Task Start()
    {
        var rows = await _executor.Worker(() => _repository.All());
        foreach (var r in rows) _stored[r.Id] = r;
        _subscriptions.Add(_bus.Subscribe(new[] { EngineEventType.LeaseChanged }, e => OnLeases(e)));
        _subscriptions.Add(_bus.Subscribe(new[] { EngineEventType.ConfigChanged }, _ => OnConfig()));
        _started = true;
        _monitor.Start(e => _executor.Post(() => OnDeviceEvent(e)));
        Poll();
        await Scan(asked: true);
    }

    public void Stop()
    {
        _started = false;
        _monitor.Stop();
        _poll?.Cancel();
        _poll = null;
        foreach (var s in _subscriptions) s.Cancel();
        _subscriptions.Clear();
    }

    /// <summary>The connected drives in MakeMKV's order, then the configured ones that aren't connected (on the
    /// executor).</summary>
    public IReadOnlyList<DriveDto> Drives() => Listed().Select(d => d.Shown ?? Dto(d)).ToList();

    /// <summary>Scans now; completes when that scan is applied.</summary>
    public Task Rescan() => Scan(asked: true);

    /// <summary>Ejects the drive's disc (drive.notFound when it isn't connected; the DriveControl's failure, such as
    /// drive.ejectFailed), then scans 3 s later.</summary>
    public async Task Eject(string driveId)
    {
        var device = Device(Connected(driveId));
        await _executor.Worker(() => _control.Eject(device));
        ScanIn(ScanAfterEject);
    }

    /// <summary>Closes the drive's tray, then scans 5 s later; the first scan 4 s after the command checks that it
    /// closed.</summary>
    public async Task CloseTray(string driveId)
    {
        var drive = Connected(driveId);
        _trayAsked[drive.Id] = _clock.Now();
        var device = Device(drive);
        await _executor.Worker(() => _control.CloseTray(device));
        ScanIn(ScanAfterTray);
    }

    /// <summary>Closes the tray of every connected drive without a disc, one after another, then scans 5 s later.</summary>
    public async Task CloseAll()
    {
        var drives = _drives.Where(d => d.Connected && d.State is DriveStatus.Empty or DriveStatus.TrayOpen && Device(d).Length > 0).ToList();
        var now = _clock.Now();
        foreach (var d in drives) _trayAsked[d.Id] = now;
        foreach (var d in drives)
        {
            var device = Device(d);
            try { await _executor.Worker(() => _control.CloseTray(device)); }
            catch (BroFailure) { } // the check after the scan says which trays stayed open
        }
        ScanIn(ScanAfterTray);
    }

    /// <summary>Where a drive is now, from any thread; null when it isn't connected.</summary>
    public DriveLocation? Find(string driveId)
    {
        lock (_lock) return _snapshot.GetValueOrDefault(driveId);
    }

    Drive Connected(string driveId) => _drives.FirstOrDefault(d => d.Id == driveId && d.Connected)
        ?? throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.DriveNotFound), ("driveId", JsonValue.Of(driveId))));

    /// <summary>MakeMKV's device, else the OS's (macOS: an empty drive has no device in MakeMKV's list).</summary>
    static string Device(Drive d) => d.Makemkv is { Device.Length: > 0 } m ? m.Device : d.OsDevice ?? "";

    void Poll()
    {
        var seconds = _config.Current().Document["automation"]?["pollIntervalSeconds"]?.AsInteger ?? 10;
        if (seconds == _pollSeconds) return;
        _pollSeconds = seconds;
        _poll?.Cancel();
        _poll = seconds > 0 ? _clock.Timer(new TimerSchedule.Every(new Duration(seconds)), () => _ = Scan(asked: false)) : null;
    }

    void ScanIn(Duration after) =>
        _clock.Timer(new TimerSchedule.At(new Instant(_clock.Now().UnixMilliseconds + (long)(after.Seconds * 1000))), () => _ = Scan(asked: true));

    /// <summary>Starts a scan, or one more after the scan that is out; a scan nobody asked for waits while an
    /// acquisition runs (unless automation.pollWhileRipping) and runs when it ends.</summary>
    Task Scan(bool asked)
    {
        if (!_started) return Task.CompletedTask;
        if (!asked && _leases.Acquisitions > 0 && _config.Current().Document["automation"]?["pollWhileRipping"]?.AsBool != true)
        {
            _deferred = true;
            return Task.CompletedTask;
        }
        var done = new TaskCompletionSource();
        if (_scanning)
        {
            _again = true;
            _next.Add(done);
        }
        else
        {
            _waiting.Add(done);
            _ = RunScan();
        }
        return done.Task;
    }

    async Task RunScan()
    {
        _scanning = true;
        var known = _drives.Where(d => d.Medium != null).ToDictionary(d => d.Id, d => (d.Medium!.Label, d.Makemkv?.Flags));
        var busy = _leases.Drives.Select(h => h.Drive).ToHashSet();
        var found = await _executor.Worker(async () =>
        {
            try
            {
                var mk = await _makemkv.ScanDrives(new CancellationSource().Token).ConfigureAwait(false);
                var os = _monitor.CurrentDrives();
                // What is on each disc that just arrived (a known disc, or one a job is using, isn't read again).
                var contents = new Dictionary<string, DiscContent>();
                foreach (var j in DriveJoin.Join(mk, os))
                    if (j.Makemkv is { State: DriveState.Inserted } m && !busy.Contains(j.DriveId)
                        && !(known.TryGetValue(j.DriveId, out var k) && k.Label == m.Label && k.Flags == m.Flags))
                    {
                        var device = m.Device.Length > 0 ? m.Device : j.Os?.Device ?? "";
                        contents[j.DriveId] = device.Length > 0 ? _control.ProbeContent(device) : DiscContent.Unknown;
                    }
                return new Found(mk, os, contents, null);
            }
            catch (BroFailure f)
            {
                return new Found(Array.Empty<MakemkvDrive>(), Array.Empty<OsDrive>(), new(), f.Error);
            }
        });
        var records = Apply(found);
        if (records.Count > 0)
            await _executor.Worker(async () =>
            {
                foreach (var r in records) await _repository.Upsert(r).ConfigureAwait(false);
                return true;
            });
        var waiting = _waiting;
        _waiting = _next;
        _next = new();
        foreach (var w in waiting) w.SetResult();
        _scanning = false;
        if (_again)
        {
            _again = false;
            _ = RunScan();
        }
    }

    /// <summary>The scan's drives into the state, with the events; the rows to store.</summary>
    List<DriveRecord> Apply(Found found)
    {
        var config = _config.Current();
        if (found.Failure is { } failure)
        {
            var reason = failure.Params.Members.Count > 0
                ? JsonValue.Of(("code", JsonValue.Of(failure.Code)), ("params", failure.Params))
                : JsonValue.Of(("code", JsonValue.Of(failure.Code)));
            _problems.Raise(new BroMessage(MessageCode.MakemkvDriveScanFailed, Severity.Error, ("reason", reason)));
            return new List<DriveRecord>();
        }
        _problems.Clear(MessageCode.MakemkvDriveScanFailed);
        var now = _clock.Now();
        var first = !_firstDone;
        _firstDone = true;
        var seen = new HashSet<string>();
        var busy = _leases.Drives.Select(h => h.Drive).ToHashSet();
        var order = new List<Drive>();
        var removed = new List<(Drive Drive, long Generation)>();
        var arrived = new List<Drive>();
        var records = new List<DriveRecord>();
        foreach (var j in DriveJoin.Join(found.Makemkv, found.Os))
        {
            if (j.Makemkv is not { } m) continue; // an OS drive MakeMKV doesn't list
            var drive = _drives.FirstOrDefault(d => d.Id == j.DriveId);
            if (drive == null)
            {
                drive = new Drive { Id = j.DriveId };
                _drives.Add(drive);
            }
            seen.Add(drive.Id);
            order.Add(drive);
            if (busy.Contains(drive.Id) && drive.Connected) continue; // a job holds it: MakeMKV can't see it
            var wasConnected = drive.Connected;
            drive.Makemkv = m;
            drive.OsDevice = j.Os?.Device ?? drive.OsDevice;
            drive.Connected = true;
            drive.State = Status(m.State);
            var old = drive.Medium;
            if (m.State == DriveState.Inserted)
            {
                var same = old != null && old.Label == m.Label && old.Flags.SequenceEqual(MediumDto.FlagNames(m.Flags));
                if (same) drive.Medium = old! with { MountPath = j.Os?.MountPath ?? old.MountPath };
                else
                {
                    if (old != null) removed.Add((drive, old.Generation));
                    var generation = _generations[drive.Id] = _generations.GetValueOrDefault(drive.Id) + 1;
                    drive.Medium = new MediumDto(generation, m.Label, MediumDto.TypeOf(m.Flags), DiscFlags.TypeText(m.Flags), MediumDto.FlagNames(m.Flags),
                        found.Contents.GetValueOrDefault(drive.Id, DiscContent.Unknown), j.Os?.MountPath);
                    if (!first) arrived.Add(drive);
                }
            }
            else
            {
                if (old != null) removed.Add((drive, old.Generation));
                drive.Medium = null;
            }
            var configId = ConfigId(config, drive);
            var stored = _stored.GetValueOrDefault(drive.Id);
            if (!wasConnected || stored == null || stored.LastDevice != Device(drive) || stored.ConfigId != configId)
            {
                var record = new DriveRecord(drive.Id, m.Identification, DriveJoin.ShortModel(m.Identification), Device(drive), configId,
                    stored?.FirstSeenAt ?? now, now);
                _stored[drive.Id] = record;
                records.Add(record);
            }
        }
        // A connected drive left out of the scan stays as it was, unless the OS no longer lists its device.
        foreach (var d in _drives.Where(d => d.Connected && !seen.Contains(d.Id)))
            if (d.OsDevice != null && found.Os.All(o => o.Device != d.OsDevice)) Disconnect(d, removed);
        var sorted = _drives.OrderBy(d => Rank(d, order)).ToList(); // stable
        _drives.Clear();
        _drives.AddRange(sorted);

        Publish(config, removed, arrived);
        CheckTrays(config, now);
        return records;
    }

    /// <summary>A tray closed at least 4 s ago that is still open is reported; one that isn't open any more, cleared.</summary>
    void CheckTrays(Config config, Instant now)
    {
        foreach (var (drive, asked) in _trayAsked.ToList())
        {
            if (now.UnixMilliseconds - asked.UnixMilliseconds < TrayCheckAfter.Seconds * 1000) continue;
            _trayAsked.Remove(drive);
            if (_drives.FirstOrDefault(d => d.Id == drive) is { Connected: true, State: DriveStatus.TrayOpen } open)
                _problems.Raise(new BroMessage(MessageCode.DriveTrayStillOpen, Severity.Warning, ("drive", JsonValue.Of(Name(config, open)))),
                    new ProblemScope(ProblemScopeKind.Drive, open.Id));
        }
        foreach (var d in _drives.Where(d => d.State != DriveStatus.TrayOpen || !d.Connected))
            _problems.Clear(MessageCode.DriveTrayStillOpen, new ProblemScope(ProblemScopeKind.Drive, d.Id));
    }

    /// <summary>The drive is gone: its disc too.</summary>
    void Disconnect(Drive d, List<(Drive, long)> removed)
    {
        if (d.Medium != null) removed.Add((d, d.Medium.Generation));
        d.Medium = null;
        d.Connected = false;
        d.State = DriveStatus.Unavailable;
    }

    static int Rank(Drive d, List<Drive> order) => order.IndexOf(d) is var i and >= 0 ? i : order.Count + (d.Makemkv?.Index ?? 0);

    /// <summary>mediumRemoved, driveChanged and mediumArrived per drive, in the list's order; then driveRemoved for the
    /// drives without an entry that are gone, and the snapshot.</summary>
    void Publish(Config config, List<(Drive Drive, long Generation)> removed, List<Drive> arrived)
    {
        foreach (var d in _drives.ToList())
        {
            foreach (var (_, generation) in removed.Where(r => r.Drive == d))
                _bus.Publish(EngineEventType.MediumRemoved, JsonValue.Of(("driveId", JsonValue.Of(d.Id)), ("generation", JsonValue.Of(generation))));
            if (!d.Connected && ConfigId(config, d) == null)
            {
                _drives.Remove(d);
                if (d.Shown != null) _bus.Publish(EngineEventType.DriveRemoved, JsonValue.Of(("driveId", JsonValue.Of(d.Id))));
                continue;
            }
            var dto = Dto(d);
            if (d.Shown == null || !DriveDto.Json(d.Shown).Equals(DriveDto.Json(dto)))
            {
                d.Shown = dto;
                _bus.Publish(EngineEventType.DriveChanged, JsonValue.Of(("drive", DriveDto.Json(dto))));
            }
            if (arrived.Contains(d))
                _bus.Publish(EngineEventType.MediumArrived, JsonValue.Of(("driveId", JsonValue.Of(d.Id)), ("generation", JsonValue.Of(d.Medium!.Generation)),
                    ("medium", MediumDto.Json(d.Medium))));
        }
        var snapshot = _drives.Where(d => d.Connected).ToDictionary(d => d.Id, d => new DriveLocation(d.Makemkv?.Index, Device(d), ConfigId(config, d),
            d.Medium?.Generation, d.Medium?.Label ?? "", d.Medium != null ? d.Makemkv?.Flags : null));
        lock (_lock) _snapshot = snapshot;
    }

    /// <summary>The connected drives, then the configured entries without a connected drive (an id from the stored
    /// drive with that entry, else from the entry's match).</summary>
    IEnumerable<Drive> Listed()
    {
        var config = _config.Current();
        var listed = _drives.Where(d => d.Connected || ConfigId(config, d) != null).ToList();
        foreach (var entry in Config.Drives(config))
        {
            if (listed.Any(d => ConfigId(config, d) == entry.Id)) continue;
            var id = _stored.Values.OrderBy(r => r.Id, StringComparer.Ordinal).FirstOrDefault(r => r.ConfigId == entry.Id)?.Id
                ?? DriveJoin.DriveId(entry.Match.DriveName, entry.Match.DevicePath);
            listed.Add(new Drive { Id = id, EntryId = entry.Id });
        }
        return listed;
    }

    DriveDto Dto(Drive d)
    {
        var config = _config.Current();
        var configId = ConfigId(config, d);
        var entry = Config.Drives(config).FirstOrDefault(e => e.Id == configId);
        var holder = _leases.Drives.FirstOrDefault(h => h.Drive == d.Id);
        var profile = entry?.Profile ?? config.Document["defaultProfile"]?.AsString ?? "default";
        return new DriveDto(d.Id, configId, Name(config, d), d.Makemkv?.Identification ?? _stored.GetValueOrDefault(d.Id)?.Identification ?? "",
            d.Connected ? Device(d) : "", d.Connected ? d.Makemkv?.Index : null, d.Connected, d.Connected ? d.State : DriveStatus.Unavailable,
            d.Connected ? d.Medium : null, holder != null, holder?.Job, profile, Config.Automation(config, configId)["ripOnInsert"]?.AsBool == true);
    }

    string? ConfigId(Config config, Drive d)
    {
        if (d.Makemkv is { } m) return DriveJoin.EntryFor(Config.Drives(config), m)?.Id;
        if (d.EntryId != null) return d.EntryId;
        var stored = _stored.GetValueOrDefault(d.Id)?.ConfigId;
        return Config.Drives(config).Any(e => e.Id == stored) ? stored : null;
    }

    string Name(Config config, Drive d)
    {
        var configId = ConfigId(config, d);
        if (Config.Drives(config).FirstOrDefault(e => e.Id == configId) is { } entry) return entry.Name;
        return DriveJoin.ShortModel(d.Makemkv?.Identification ?? _stored.GetValueOrDefault(d.Id)?.Identification ?? "");
    }

    static DriveStatus Status(DriveState state) => state switch
    {
        DriveState.EmptyClosed => DriveStatus.Empty,
        DriveState.EmptyOpen => DriveStatus.TrayOpen,
        DriveState.Inserted => DriveStatus.Present,
        DriveState.Loading => DriveStatus.Loading,
        DriveState.Unmounting => DriveStatus.Unmounting,
        _ => DriveStatus.Unavailable,
    };

    void OnLeases(EngineEvent e)
    {
        if (LeaseSnapshot.Parse(e.Data) is not { } leases) return;
        _leases = leases;
        Publish(_config.Current(), new(), new());
        if (_deferred && leases.Acquisitions == 0)
        {
            _deferred = false;
            _ = Scan(asked: false);
        }
    }

    void OnConfig()
    {
        Poll();
        Publish(_config.Current(), new(), new());
    }

    void OnDeviceEvent(DeviceEvent e)
    {
        if (!_started) return;
        switch (e)
        {
            case DeviceEvent.Mounted mounted:
                SetMount(mounted.Device, mounted.Path);
                return;
            case DeviceEvent.Unmounted unmounted:
                SetMount(unmounted.Device, null);
                return;
            case DeviceEvent.DriveVanished vanished:
                var removed = new List<(Drive, long)>();
                foreach (var d in _drives.Where(d => d.Connected && Same(d, vanished.Device)).ToList()) Disconnect(d, removed);
                Publish(_config.Current(), removed, new());
                break;
        }
        _ = Scan(asked: false);
    }

    void SetMount(string device, string? path)
    {
        foreach (var d in _drives.Where(d => d.Connected && d.Medium != null && Same(d, device)))
            d.Medium = d.Medium! with { MountPath = path };
        Publish(_config.Current(), new(), new());
    }

    /// <summary>The OS's device, or the one the drive is used by.</summary>
    static bool Same(Drive d, string device) => d.OsDevice == device || Device(d) == device;
}
