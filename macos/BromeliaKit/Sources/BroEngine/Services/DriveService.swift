import BroDomain
import BroFoundation
import BroPorts
import Foundation

/// The optical drives (plan §11.1, §15.2), on the executor. A scan (MakeMKV's drive list and the OS's, on a worker, one
/// at a time) runs at start, every automation.pollIntervalSeconds, on the OS's device events and after a tray command;
/// while an acquisition lease is held only asked-for scans run, unless automation.pollWhileRipping. DriveJoin pairs the
/// two lists; a drive's id comes from MakeMKV's identification. Each drive counts its discs: a disc that arrives gets the
/// next generation and mediumArrived (not the discs already in at start, which never start a rip), one that leaves
/// mediumRemoved. A drive a job holds keeps what it had (MakeMKV can't see a drive in use), and a drive left out of a
/// scan keeps it until the OS says it's gone; a drive a scan found held is scanned again once the job gives it back (an
/// eject while a job holds it). Configured drives that aren't connected stay listed (connected false);
/// others are removed. Close tray checks, in the first scan 4 s later, that the tray closed (drive.trayStillOpen: a
/// drive without a tray motor). As the `DriveLookup` of the steps it gives a snapshot, from any thread.
/// shared/fixtures/engine/drive-service.cases.json.
@EngineActor
public final class DriveService: DriveLookup {
    private nonisolated static let trayCheckAfterMs: Int64 = 4000
    private nonisolated static let scanAfterTray = Duration(seconds: 5)
    private nonisolated static let scanAfterEject = Duration(seconds: 3)

    private nonisolated let executor: EngineExecutor
    private nonisolated let bus: EventBus
    private nonisolated let problems: ProblemService
    private nonisolated let config: any ConfigSource
    private nonisolated let clock: EngineClock
    private nonisolated let monitor: any DeviceMonitor
    private nonisolated let control: any DriveControl
    private nonisolated let makemkv: any Makemkv
    private nonisolated let repository: any DriveRepository

    private final class Drive {
        let id: String
        /// A configured entry no connected drive matches (listed only).
        var entryId: String?
        var makemkv: MakemkvDrive?
        var osDevice: String?
        var connected = false
        var state = DriveStatus.unavailable
        var medium: MediumDto?
        var shown: DriveDto?

        init(id: String, entryId: String? = nil) {
            self.id = id
            self.entryId = entryId
        }
    }

    /// What a scan's worker found.
    private struct Found: Sendable {
        var makemkv: [MakemkvDrive]
        var os: [OsDrive]
        var contents: [String: DiscContent]
        var failure: BroError?
    }

    private var list: [Drive] = []
    private var generations: [String: Int64] = [:]
    private var stored: [String: DriveRecord] = [:]
    private var trayAsked: [(drive: String, at: Instant)] = []
    private var leases = LeaseSnapshot.empty
    private var started = false, firstDone = false, scanning = false, again = false, deferred = false
    /// Drives a scan left as they were because a job held them: scanned again once it gives them back.
    private var heldInScan: Set<String> = []
    private var waiting: [Resumption<Void>] = [], next: [Resumption<Void>] = []
    private var poll: EngineTimer?
    private var pollSeconds: Int64 = -1
    private var subscriptions: [Subscription] = []

    private final class Shared: @unchecked Sendable {
        let lock = NSLock()
        var snapshot: [String: DriveLocation] = [:]
    }
    private nonisolated let shared = Shared()

    public init(executor: EngineExecutor, bus: EventBus, problems: ProblemService, config: any ConfigSource, clock: EngineClock,
                monitor: any DeviceMonitor, control: any DriveControl, makemkv: any Makemkv, repository: any DriveRepository) {
        self.executor = executor
        self.bus = bus
        self.problems = problems
        self.config = config
        self.clock = clock
        self.monitor = monitor
        self.control = control
        self.makemkv = makemkv
        self.repository = repository
    }

    /// Reads the stored drives, follows leases, the configuration and the OS's events, and scans; returns when the first
    /// scan is applied.
    public func start() async throws(BroError) {
        let repository = repository
        let rows = try await executor.worker { () async throws(BroError) in try await repository.all() }
        for r in rows { stored[r.id] = r }
        subscriptions.append(bus.subscribe(types: [.leaseChanged]) { [unowned self] e in onLeases(e) })
        subscriptions.append(bus.subscribe(types: [.configChanged]) { [unowned self] _ in onConfig() })
        started = true
        let executor = executor
        monitor.start { [weak self] e in _ = executor.post { self?.onDeviceEvent(e) } }
        schedulePoll()
        await scan(asked: true)
    }

    public func stop() {
        started = false
        monitor.stop()
        poll?.cancel()
        poll = nil
        for s in subscriptions { s.cancel() }
        subscriptions = []
    }

    /// The connected drives in MakeMKV's order, then the configured ones that aren't connected.
    public func drives() -> [DriveDto] { listed().map { $0.shown ?? dto($0) } }

    /// Scans now; returns when that scan is applied.
    public func rescan() async { await scan(asked: true) }

    /// Ejects the drive's disc (drive.notFound when it isn't connected; the DriveControl's failure, such as
    /// drive.ejectFailed), then scans 3 s later.
    public func eject(_ driveId: String) async throws(BroError) {
        let device = Self.device(try connected(driveId))
        let control = control
        try await executor.worker { () async throws(BroError) in try await control.eject(device) }
        scan(in: Self.scanAfterEject)
    }

    /// Closes the drive's tray, then scans 5 s later; the first scan 4 s after the command checks that it closed.
    public func closeTray(_ driveId: String) async throws(BroError) {
        let drive = try connected(driveId)
        ask(drive.id, clock.now())
        let device = Self.device(drive)
        let control = control
        try await executor.worker { () async throws(BroError) in try await control.closeTray(device) }
        scan(in: Self.scanAfterTray)
    }

    /// Closes the tray of every connected drive without a disc, one after another, then scans 5 s later.
    public func closeAll() async {
        let chosen = list.filter { $0.connected && ($0.state == .empty || $0.state == .trayOpen) && !Self.device($0).isEmpty }
        let now = clock.now()
        for d in chosen { ask(d.id, now) }
        let control = control
        for d in chosen {
            let device = Self.device(d)
            // the check after the scan says which trays stayed open
            try? await executor.worker { () async throws(BroError) in try await control.closeTray(device) }
        }
        scan(in: Self.scanAfterTray)
    }

    /// Where a drive is now, from any thread; nil when it isn't connected.
    public nonisolated func find(_ driveId: String) -> DriveLocation? { shared.lock.withLock { shared.snapshot[driveId] } }

    private func ask(_ id: String, _ at: Instant) {
        trayAsked.removeAll { $0.drive == id }
        trayAsked.append((id, at))
    }

    private func connected(_ driveId: String) throws(BroError) -> Drive {
        guard let d = list.first(where: { $0.id == driveId && $0.connected }) else {
            throw BroError(MessageCode.driveNotFound.rawValue, [("driveId", .string(driveId))])
        }
        return d
    }

    /// MakeMKV's device, else the OS's (macOS: an empty drive has no device in MakeMKV's list).
    private static func device(_ d: Drive) -> String {
        if let m = d.makemkv, !m.device.isEmpty { return m.device }
        return d.osDevice ?? ""
    }

    private func schedulePoll() {
        let seconds = config.current().document["automation"]?["pollIntervalSeconds"]?.int ?? 10
        if seconds == pollSeconds { return }
        pollSeconds = seconds
        poll?.cancel()
        poll = seconds > 0 ? clock.timer(.every(interval: Duration(seconds: Double(seconds)))) { [unowned self] in startScan(asked: false) } : nil
    }

    private func scan(in after: Duration) {
        let at = Instant(unixMilliseconds: clock.now().unixMilliseconds + Int64(after.seconds * 1000))
        _ = clock.timer(.at(instant: at)) { [unowned self] in startScan(asked: true) }
    }

    /// A scan nobody waits for.
    private func startScan(asked: Bool) {
        executor.spawn { [unowned self] in await scan(asked: asked) }
    }

    /// Starts a scan, or one more after the scan that is out; a scan nobody asked for waits while an acquisition runs
    /// (unless automation.pollWhileRipping) and runs when it ends.
    private func scan(asked: Bool) async {
        guard started else { return }
        if !asked && leases.acquisitions > 0 && config.current().document["automation"]?["pollWhileRipping"]?.bool != true {
            deferred = true
            return
        }
        await executor.suspend { (done: Resumption<Void>) in
            if scanning {
                again = true
                next.append(done)
            } else {
                waiting.append(done)
                executor.spawn { [unowned self] in await runScan() }
            }
        }
    }

    private func runScan() async {
        scanning = true
        var known: [String: (label: String, flags: DiscFlags?)] = [:]
        for d in list { if let m = d.medium { known[d.id] = (m.label, d.makemkv?.flags) } }
        let busy = Set(leases.drives.map(\.drive))
        let makemkv = makemkv, monitor = monitor, control = control
        let knownCopy = known
        let found = (try? await executor.worker { () async throws(BroError) -> Found in
            do throws(BroError) {
                let mk = try await makemkv.scanDrives(CancellationSource().token)
                let os = monitor.currentDrives()
                // What is on each disc that just arrived (a known disc, or one a job is using, isn't read again).
                var contents: [String: DiscContent] = [:]
                for j in DriveJoin.join(mk, osDrives: os) {
                    guard let m = j.makemkv, m.state == .inserted, !busy.contains(j.driveId) else { continue }
                    if let k = knownCopy[j.driveId], k.label == m.label, k.flags == m.flags { continue }
                    let device = !m.device.isEmpty ? m.device : j.os?.device ?? ""
                    contents[j.driveId] = device.isEmpty ? .unknown : control.probeContent(device)
                }
                return Found(makemkv: mk, os: os, contents: contents, failure: nil)
            } catch {
                return Found(makemkv: [], os: [], contents: [:], failure: error)
            }
        }) ?? Found(makemkv: [], os: [], contents: [:], failure: nil)
        let records = apply(found)
        if !records.isEmpty {
            let repository = repository
            try? await executor.worker { () async throws(BroError) in
                for r in records { try await repository.upsert(r) }
            }
        }
        let done = waiting
        waiting = next
        next = []
        for w in done { w.resume(returning: ()) }
        scanning = false
        if again {
            again = false
            executor.spawn { [unowned self] in await runScan() }
        }
    }

    /// The scan's drives into the state, with the events; the rows to store.
    private func apply(_ found: Found) -> [DriveRecord] {
        let config = config.current()
        if let failure = found.failure {
            let reason: JsonValue = failure.params.isEmpty
                ? .object([("code", .string(failure.code))]) : .object([("code", .string(failure.code)), ("params", .object(failure.params))])
            _ = problems.raise(BroMessage(.makemkvDriveScanFailed, [("reason", reason)], severity: .error))
            return []
        }
        problems.clear(.makemkvDriveScanFailed)
        let now = clock.now()
        let first = !firstDone
        firstDone = true
        var seen = Set<String>()
        let busy = Set(leases.drives.map(\.drive))
        var order: [Drive] = []
        var removed: [(drive: Drive, generation: Int64)] = []
        var arrived: [Drive] = []
        var records: [DriveRecord] = []
        for j in DriveJoin.join(found.makemkv, osDrives: found.os) {
            guard let m = j.makemkv else { continue } // an OS drive MakeMKV doesn't list
            let drive: Drive
            if let d = list.first(where: { $0.id == j.driveId }) {
                drive = d
            } else {
                drive = Drive(id: j.driveId)
                list.append(drive)
            }
            seen.insert(drive.id)
            order.append(drive)
            if busy.contains(drive.id) && drive.connected { // a job holds it: MakeMKV can't see it
                heldInScan.insert(drive.id)
                continue
            }
            let wasConnected = drive.connected
            drive.makemkv = m
            drive.osDevice = j.os?.device ?? drive.osDevice
            drive.connected = true
            drive.state = Self.status(m.state)
            let old = drive.medium
            if m.state == .inserted {
                if let old, old.label == m.label, old.flags == MediumDto.flagNames(m.flags) {
                    var same = old
                    same.mountPath = j.os?.mountPath ?? old.mountPath
                    drive.medium = same
                } else {
                    if let old { removed.append((drive, old.generation)) }
                    let generation = (generations[drive.id] ?? 0) + 1
                    generations[drive.id] = generation
                    drive.medium = MediumDto(generation: generation, label: m.label, type: MediumDto.typeOf(m.flags), typeText: DiscFlags.typeText(m.flags),
                                             flags: MediumDto.flagNames(m.flags), content: found.contents[drive.id] ?? .unknown, mountPath: j.os?.mountPath)
                    if !first { arrived.append(drive) }
                }
            } else {
                if let old { removed.append((drive, old.generation)) }
                drive.medium = nil
            }
            let configId = self.configId(config, drive)
            let row = stored[drive.id]
            if !wasConnected || row == nil || row?.lastDevice != Self.device(drive) || row?.configId != configId {
                let record = DriveRecord(id: drive.id, identification: m.identification, model: DriveJoin.shortModel(m.identification),
                                         lastDevice: Self.device(drive), configId: configId, firstSeenAt: row?.firstSeenAt ?? now, lastSeenAt: now)
                stored[drive.id] = record
                records.append(record)
            }
        }
        // A connected drive left out of the scan stays as it was, unless the OS no longer lists its device.
        for d in list where d.connected && !seen.contains(d.id) {
            if let os = d.osDevice, !found.os.contains(where: { $0.device == os }) { disconnect(d, &removed) }
        }
        list = Self.sorted(list, order)
        publish(config, removed, arrived)
        checkTrays(config, now)
        return records
    }

    /// The drives in the scan's order, then the others (by MakeMKV's index).
    private static func sorted(_ list: [Drive], _ order: [Drive]) -> [Drive] {
        func rank(_ d: Drive) -> Int {
            if let i = order.firstIndex(where: { $0 === d }) { return i }
            return order.count + (d.makemkv?.index ?? 0)
        }
        return list.enumerated().sorted { (rank($0.element), $0.offset) < (rank($1.element), $1.offset) }.map(\.element)
    }

    /// A tray closed at least 4 s ago that is still open is reported; one that isn't open any more, cleared.
    private func checkTrays(_ config: Config, _ now: Instant) {
        for asked in trayAsked where now.unixMilliseconds - asked.at.unixMilliseconds >= Self.trayCheckAfterMs {
            trayAsked.removeAll { $0.drive == asked.drive }
            if let open = list.first(where: { $0.id == asked.drive && $0.connected && $0.state == .trayOpen }) {
                _ = problems.raise(BroMessage(.driveTrayStillOpen, [("drive", .string(name(config, open)))], severity: .warning),
                                   scope: ProblemScope(kind: .drive, id: open.id))
            }
        }
        for d in list where d.state != .trayOpen || !d.connected {
            problems.clear(.driveTrayStillOpen, scope: ProblemScope(kind: .drive, id: d.id))
        }
    }

    /// The drive is gone: its disc too.
    private func disconnect(_ d: Drive, _ removed: inout [(drive: Drive, generation: Int64)]) {
        if let m = d.medium { removed.append((d, m.generation)) }
        d.medium = nil
        d.connected = false
        d.state = .unavailable
    }

    /// mediumRemoved, driveChanged and mediumArrived per drive, in the list's order; then driveRemoved for the drives
    /// without an entry that are gone, and the snapshot.
    private func publish(_ config: Config, _ removed: [(drive: Drive, generation: Int64)], _ arrived: [Drive]) {
        for d in list {
            for r in removed where r.drive === d {
                _ = bus.publish(.mediumRemoved, data: .object([("driveId", .string(d.id)), ("generation", .integer(r.generation))]))
            }
            if !d.connected && configId(config, d) == nil {
                list.removeAll { $0 === d }
                if d.shown != nil { _ = bus.publish(.driveRemoved, data: .object([("driveId", .string(d.id))])) }
                continue
            }
            let now = dto(d)
            if d.shown == nil || DriveDto.json(d.shown!) != DriveDto.json(now) {
                d.shown = now
                _ = bus.publish(.driveChanged, data: .object([("drive", DriveDto.json(now))]))
            }
            if arrived.contains(where: { $0 === d }), let m = d.medium {
                _ = bus.publish(.mediumArrived, data: .object([("driveId", .string(d.id)), ("generation", .integer(m.generation)),
                                                               ("medium", MediumDto.json(m))]))
            }
        }
        var snapshot: [String: DriveLocation] = [:]
        for d in list where d.connected {
            snapshot[d.id] = DriveLocation(makemkvIndex: d.makemkv?.index, device: Self.device(d), configId: configId(config, d),
                                           generation: d.medium?.generation, label: d.medium?.label ?? "",
                                           flags: d.medium != nil ? d.makemkv?.flags : nil)
        }
        shared.lock.withLock { shared.snapshot = snapshot }
    }

    /// The connected drives, then the configured entries without a connected drive (an id from the stored drive with that
    /// entry, else from the entry's match).
    private func listed() -> [Drive] {
        let config = config.current()
        var listed = list.filter { $0.connected || configId(config, $0) != nil }
        for entry in Config.drives(config) where !listed.contains(where: { configId(config, $0) == entry.id }) {
            let id = stored.values.sorted { $0.id < $1.id }.first(where: { $0.configId == entry.id })?.id
                ?? DriveJoin.driveId(entry.match.driveName, device: entry.match.devicePath)
            listed.append(Drive(id: id, entryId: entry.id))
        }
        return listed
    }

    private func dto(_ d: Drive) -> DriveDto {
        let config = config.current()
        let configId = configId(config, d)
        let entry = Config.drives(config).first { $0.id == configId }
        let holder = leases.drives.first { $0.drive == d.id }
        let profile = entry?.profile ?? config.document["defaultProfile"]?.string ?? "default"
        return DriveDto(id: d.id, configId: configId, name: name(config, d),
                        identification: d.makemkv?.identification ?? stored[d.id]?.identification ?? "",
                        device: d.connected ? Self.device(d) : "", makemkvIndex: d.connected ? d.makemkv?.index : nil, connected: d.connected,
                        state: d.connected ? d.state : .unavailable, medium: d.connected ? d.medium : nil, busy: holder != nil, jobId: holder?.job,
                        profileId: profile, ripOnInsert: Config.automation(config, configId)["ripOnInsert"]?.bool == true)
    }

    private func configId(_ config: Config, _ d: Drive) -> String? {
        if let m = d.makemkv { return DriveJoin.entryFor(Config.drives(config), drive: m)?.id }
        if let entryId = d.entryId { return entryId }
        let stored = stored[d.id]?.configId
        return Config.drives(config).contains { $0.id == stored } ? stored : nil
    }

    private func name(_ config: Config, _ d: Drive) -> String {
        let configId = configId(config, d)
        if let entry = Config.drives(config).first(where: { $0.id == configId }) { return entry.name }
        return DriveJoin.shortModel(d.makemkv?.identification ?? stored[d.id]?.identification ?? "")
    }

    private static func status(_ state: DriveState) -> DriveStatus {
        switch state {
        case .emptyClosed: return .empty
        case .emptyOpen: return .trayOpen
        case .inserted: return .present
        case .loading: return .loading
        case .unmounting: return .unmounting
        case .noDrive: return .unavailable
        }
    }

    private func onLeases(_ e: EngineEvent) {
        guard let leases = LeaseSnapshot.parse(e.data) else { return }
        self.leases = leases
        publish(config.current(), [], [])
        let given = heldInScan.filter { id in !leases.drives.contains { $0.drive == id } }
        heldInScan.subtract(given)
        if (deferred && leases.acquisitions == 0) || !given.isEmpty {
            deferred = false
            startScan(asked: false)
        }
    }

    private func onConfig() {
        schedulePoll()
        publish(config.current(), [], [])
    }

    private func onDeviceEvent(_ e: DeviceEvent) {
        guard started else { return }
        switch e {
        case .mounted(let device, let path):
            setMount(device, path)
            return
        case .unmounted(let device):
            setMount(device, nil)
            return
        case .driveVanished(let device):
            var removed: [(drive: Drive, generation: Int64)] = []
            for d in list where d.connected && Self.same(d, device) { disconnect(d, &removed) }
            publish(config.current(), removed, [])
        default:
            break
        }
        startScan(asked: false)
    }

    private func setMount(_ device: String, _ path: String?) {
        for d in list where d.connected && d.medium != nil && Self.same(d, device) { d.medium!.mountPath = path }
        publish(config.current(), [], [])
    }

    /// The OS's device, or the one the drive is used by.
    private static func same(_ d: Drive, _ device: String) -> Bool { d.osDevice == device || Self.device(d) == device }
}
