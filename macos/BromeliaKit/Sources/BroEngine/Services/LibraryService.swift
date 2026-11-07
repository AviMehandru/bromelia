import BroDomain
import BroFoundation
import BroPorts
import Foundation

/// The libraries of the configuration (plan §11.1, §22). A library is online while its folder holds the
/// .bromelia-library marker it was attached with: the markers are checked at start, every 30 s and on `check()` (mount
/// events), each on its own worker; an offline library is blocked in the scheduler and raised as a problem. A check
/// that hasn't come back by the next tick (a share that froze) counts as offline and isn't started again until it comes
/// back. As the `LibraryPort` of the steps it gives staging folders and runs the commit protocol (§22.3 steps 4–6) and
/// its roll forward (§15.8) on workers, one at a time per unit: a commit waits while a roll forward of its unit runs
/// (recovery's may outlast its 30 s at startup on a slow share). State is touched only on the executor; the steps read
/// a snapshot. As the `CatalogQueries` of the steps it asks the catalogue on a worker. Paths are compared and split by
/// their UTF-8 bytes, as a disk names them. shared/fixtures/engine/library-service.cases.json.
@EngineActor
public final class LibraryService: LibraryPort, CatalogQueries {
    private nonisolated static let fileMode = 0o644
    private nonisolated static let staging = ".bromelia"
    private nonisolated static let quarantineFolder = "_Quarantine"
    private nonisolated static let incompleteNote = "INCOMPLETE.txt"
    /// The media server layout's folder for backups (Layouts); servers are kept out of it by the ignore files.
    private nonisolated static let backupFolder = "Backup"
    /// .plexignore for Plex, .ignore for Jellyfin and Emby.
    private nonisolated static let ignoreFiles: [(name: String, text: String)] = [(".plexignore", "*\n"), (".ignore", "")]
    private nonisolated static let slash = UInt8(ascii: "/")
    private nonisolated static let reasonCodes: [MessageCode] = [.libraryOffline, .libraryNotAttached, .libraryMarkerMismatch]

    private nonisolated let executor: EngineExecutor
    private let bus: EventBus
    private let problems: ProblemService
    private let scheduler: Scheduler
    private let config: any ConfigSource
    private nonisolated let clock: EngineClock
    private nonisolated let fs: any FileSystem
    private nonisolated let rows: any LibraryRepository
    private nonisolated let units: any UnitRepository
    private nonisolated let catalog: any CatalogRepository
    private nonisolated let crash: (@Sendable (String) throws(BroError) -> Void)?

    @EngineActor
    private final class Entry {
        let id: String
        var name: String
        let path: String
        var known = false
        var online = false
        var reason: BroMessage?
        var free: Int64?
        var seen: Instant?
        var inFlight = false

        init(id: String, name: String, path: String) {
            self.id = id
            self.name = name
            self.path = path
        }
    }

    /// What a marker check found.
    private struct Found: Sendable {
        var reason: BroMessage?
        var free: Int64? = nil
        var seen: Instant? = nil
    }

    /// Checks applied together, in configuration order, when all have come back or at the next tick.
    @EngineActor
    private final class Round {
        var pending: Set<ObjectIdentifier> = []
        var results: [ObjectIdentifier: Found] = [:]
        var waiters: [Resumption<Void>] = []
    }

    /// The snapshot the steps read, and the units a commit or roll forward is writing, each with the ones waiting for it
    /// (woken when it is done, or one by its cancel).
    private final class Shared: @unchecked Sendable {
        let lock = NSLock()
        var snapshot: [String: (name: String, path: String, reason: BroMessage?)] = [:]
        var committing: [Id: [(id: Int, wake: CheckedContinuation<Void, Never>)]] = [:]
        var waiting = 0
        var nextWaiter = 0
    }

    private var entries: [Entry] = []
    private var round: Round?
    private var timer: EngineTimer?
    private var subscription: Subscription?
    private nonisolated let shared = Shared()

    /// `crash` is called at each crash point of the commit (commit.afterIntent, commit.afterMove:<n>, commit.beforeDone);
    /// the test root throws there to stop the commit as a killed engine would. Nil in a real engine.
    public init(executor: EngineExecutor, bus: EventBus, problems: ProblemService, scheduler: Scheduler, config: any ConfigSource,
                clock: EngineClock, fs: any FileSystem, libraries: any LibraryRepository, units: any UnitRepository,
                catalog: any CatalogRepository, crash: (@Sendable (String) throws(BroError) -> Void)? = nil) {
        self.executor = executor
        self.bus = bus
        self.problems = problems
        self.scheduler = scheduler
        self.config = config
        self.clock = clock
        self.fs = fs
        self.rows = libraries
        self.units = units
        self.catalog = catalog
        self.crash = crash
    }

    /// Reads when each library was last seen, checks every library, then every 30 s, and follows configChanged;
    /// returns when the first round is applied.
    public func start() async throws(BroError) {
        let repository = self.rows
        let stored = try await executor.worker { () async throws(BroError) in try await repository.all() }
        sync()
        for e in entries {
            if let row = stored.first(where: { $0.id == e.id && $0.path == e.path }) { e.seen = row.lastSeenAt }
        }
        subscription = bus.subscribe(types: [.configChanged]) { [unowned self] _ in onConfigChanged() }
        timer = clock.timer(.every(interval: Duration(seconds: 30))) { [unowned self] in tick() }
        await startRound()
    }

    public func stop() {
        timer?.cancel()
        timer = nil
        subscription?.cancel()
        subscription = nil
    }

    /// In configuration order (on the executor).
    public func libraries() -> [LibraryDto] { entries.map { Self.dto($0) } }

    /// Checks every marker now (mount events); while a round is still out, waits for it instead.
    public func check() async { await startRound() }

    /// Writes the marker into an existing folder (never creates it), stores the library and brings it online:
    /// library.pathMissing{path}, library.alreadyAttached{path, library}.
    public func attach(_ libraryId: String, path: String, name: String) async throws(BroError) -> LibraryDto {
        let display = name.isEmpty ? Self.lastPart(path) : name
        let now = clock.now()
        let marker = LibraryMarker.make(libraryId, path: path, name: display, created: now)
        let (fs, repository) = (self.fs, self.rows)
        let free = try await executor.worker { () async throws(BroError) -> Int64? in
            guard fs.exists(path), try fs.stat(path).isDirectory else {
                throw Self.message(.libraryPathMissing, [("path", .string(path))]).toError()
            }
            let markerPath = path + "/" + LibraryMarker.fileName
            if fs.exists(markerPath) {
                let found = Self.decode(fs, markerPath)
                let shown = (found?.name.isEmpty == false) ? found!.name : libraryId
                throw Self.message(.libraryAlreadyAttached, [("path", .string(path)), ("library", .string(shown))]).toError()
            }
            try fs.writeAtomically(markerPath, bytes: LibraryMarker.encode(marker), mode: Self.fileMode)
            try await repository.upsert(LibraryRecord(id: libraryId, name: display, path: path, markerId: marker.id, attachedAt: now, online: true,
                                                     lastSeenAt: now))
            return Self.freeBytes(fs, path)
        }
        let e: Entry
        if let found = find(libraryId), found.path == path {
            e = found
        } else {
            e = Entry(id: libraryId, name: find(libraryId)?.name ?? display, path: path)
            if let i = entries.firstIndex(where: { $0.id == libraryId }) { entries[i] = e } else { entries.append(e) }
        }
        apply(e, Found(reason: nil, free: free, seen: now))
        return Self.dto(e)
    }

    /// Takes back an `attach` whose configuration save failed (LocalEngine's attachLibrary): the marker in the stored
    /// library's folder is removed while it is still the one the row names (another marker stays), and the library is
    /// forgotten unless the configuration lists it. Nothing when there is no stored row.
    public func removeMarker(_ libraryId: String) async throws(BroError) {
        let (fs, repository) = (self.fs, self.rows)
        try await executor.worker { () async throws(BroError) in
            guard let row = try await repository.get(libraryId) else { return }
            let markerPath = row.path + "/" + LibraryMarker.fileName
            guard fs.exists(markerPath), Self.decode(fs, markerPath)?.id == row.markerId else { return }
            try fs.remove(markerPath)
        }
        sync()
    }

    /// The catalogue's committed units of the disc (CatalogQueries), asked on a worker. Any thread.
    public nonisolated func archivedBefore(_ fingerprint: String) async throws(BroError) -> [UnitRecord] {
        let catalog = self.catalog
        return try await executor.worker { () async throws(BroError) -> [UnitRecord] in try await catalog.archivedBefore(fingerprint) }
    }

    // ---- the port (any thread) ----------------------------------------------------------------------------------

    public nonisolated func require(_ libraryId: String) throws(BroError) -> LibraryHandle {
        LibraryHandle(id: libraryId, path: try online(libraryId).path)
    }

    /// The library's name and root, from the snapshot: library.notFound, or why it is offline.
    private nonisolated func online(_ libraryId: String) throws(BroError) -> (id: String, name: String, path: String) {
        let s = shared.lock.withLock { shared.snapshot[libraryId] }
        guard let s else { throw Self.message(.libraryNotFound).toError() }
        if let reason = s.reason { throw reason.toError() }
        return (libraryId, s.name, s.path)
    }

    public nonisolated func stagingDirectory(_ libraryId: String, jobId: Id) throws(BroError) -> String {
        var dir = try require(libraryId).path
        for part in [Self.staging, "staging", jobId.value] {
            dir += "/" + part
            if !fs.exists(dir) { try fs.createDirectory(dir, parentsMustExist: true) }
        }
        return dir
    }

    public nonisolated func commit(_ unit: UnitRecord, intent: CommitIntent, files: [UnitFile], cancel: CancellationToken) async throws(BroError) -> CommitResult {
        try await run(unit, intent, files, tag: nil, cancel: cancel)
    }

    public nonisolated func quarantine(_ unit: UnitRecord, intent: CommitIntent, files: [UnitFile], tag: String, cancel: CancellationToken) async throws(BroError) -> CommitResult {
        try await run(unit, intent, files, tag: tag, cancel: cancel)
    }

    /// Finishes an interrupted commit (§15.8): once no other commit of the unit runs, the marker checked, then what is left
    /// of the unit's open intent (read again: `intent` may be stale) moved (a file already at its destination counts when
    /// its SHA-256 is the item's), SHA256SUMS merged, the folders synced and the unit committed, its files read from the
    /// destination's SHA256SUMS. Nil when the intent is no longer open (the job's Commit finished it meanwhile). Off the
    /// executor.
    public nonisolated func rollForward(_ intent: CommitIntent) async throws(BroError) -> CommitResult? {
        let units = self.units
        guard let unit = try await units.get(intent.unitId) else { throw Self.message(.unitNotFound).toError() }
        let lib = try await configured(unit.libraryId)
        return try await executor.worker { [self] () async throws(BroError) -> CommitResult? in
            try await claim(intent.unitId, cancel: nil)
            defer { unclaim(intent.unitId) }
            try await checkMarker(lib)
            guard let open = try await openIntent(intent.unitId) else { return nil }
            return try await complete(lib.path, open, nil)
        }
    }

    /// Commits and roll forwards waiting for another one of their unit to finish (a test or scenario runner counts them as
    /// parked when it settles: each is inside a worker the executor counts). Any thread.
    public nonisolated func waiting() -> Int { shared.lock.withLock { shared.waiting } }

    /// The folders in the library's .bromelia/staging (one per job, named by its id), in ordinal order, once the marker
    /// checks out (from the configuration itself, so it works before `start()`). Any thread but the executor.
    public nonisolated func stagingFolders(_ libraryId: String) async throws(BroError) -> [String] {
        let lib = try await configured(libraryId)
        return try await executor.worker { [self] () async throws(BroError) -> [String] in
            try await checkMarker(lib)
            let staging = lib.path + "/" + Self.staging + "/staging"
            guard try isFolder(staging) else { return [] }
            return try fs.list(staging).filter(\.isDirectory).map(\.name).sorted { Array($0.utf8).lexicographicallyPrecedes(Array($1.utf8)) }
        }
    }

    /// A staging folder no job will continue goes to <library>/_Quarantine/<ConflictNamer('<name> [INCOMPLETE]')> with
    /// `note` as INCOMPLETE.txt and `logs` (the job's log copies, by name) next to it, each written atomically before
    /// the move, so the folder arrives marked; never reused or deleted; one that holds no file is removed instead
    /// (nil), and nothing is written into it. The folders are synced. Any thread but the executor.
    public nonisolated func setAside(_ libraryId: String, folder: String, name: String, note: [UInt8],
                                     logs: [(String, [UInt8])]) async throws(BroError) -> String? {
        let lib = try await configured(libraryId)
        return try await executor.worker { [self] () async throws(BroError) -> String? in
            try await checkMarker(lib)
            let staging = lib.path + "/" + Self.staging + "/staging/" + folder
            guard try isFolder(staging) else { throw Self.notFound(staging) }
            if try walk(staging).isEmpty {
                try removeEmpty(staging)
                return nil
            }
            for (logName, bytes) in logs { try fs.writeAtomically(staging + "/" + logName, bytes: bytes, mode: Self.fileMode) }
            try fs.writeAtomically(staging + "/" + Self.incompleteNote, bytes: note, mode: Self.fileMode)
            let quarantine = lib.path + "/" + Self.quarantineFolder
            try ensureFolders(lib.path, quarantine)
            let dest = quarantine + "/" + ConflictNamer.next(name + " [INCOMPLETE]", existing: try fs.list(quarantine).map(\.name), isFolder: true)
            try fs.rename(staging, to: dest)
            try fs.syncDirectory(quarantine)
            try fs.syncDirectory(parentFolder(of: staging))
            return dest
        }
    }

    // ---- marker checks (executor) ---------------------------------------------------------------------------------

    private func sync() {
        let wanted = Config.libraries(config.current())
        for gone in entries where !wanted.contains(where: { $0.id == gone.id }) {
            scheduler.setLibraryOffline(gone.id, reason: nil)
            for code in Self.reasonCodes { problems.clear(code, scope: Self.scope(gone.id)) }
        }
        var ordered: [Entry] = []
        for w in wanted {
            let name = w.name.isEmpty ? w.id : w.name
            if let e = find(w.id), e.path == w.path {
                e.name = name
                ordered.append(e)
            } else {
                ordered.append(Entry(id: w.id, name: name, path: w.path))
            }
        }
        if let round {
            let kept = Set(ordered.map(ObjectIdentifier.init))
            round.pending = round.pending.filter { kept.contains($0) }
        }
        entries = ordered
        updateSnapshot()
        if let round, round.pending.isEmpty { close(round) }
    }

    private func onConfigChanged() {
        sync()
        executor.spawn { [self] in await startRound() }
    }

    private func tick() {
        if let round { close(round) }
        executor.spawn { [self] in await startRound() }
    }

    /// Checks every library that has no check out, in the open round (a new one when there is none); returns when
    /// that round is applied.
    private func startRound() async {
        let round = self.round ?? Round()
        self.round = round
        for e in entries where !e.inFlight {
            e.inFlight = true
            round.pending.insert(ObjectIdentifier(e))
            launch(e, round)
        }
        await executor.suspend { (r: Resumption<Void>) in
            round.waiters.append(r)
            if round.pending.isEmpty { close(round) }
        }
    }

    private func launch(_ e: Entry, _ round: Round) {
        let (id, name, path) = (e.id, e.name, e.path)
        executor.spawn { [self] in
            let found = await (try? executor.worker { [self] () async throws(BroError) -> Found in await inspect(id, name, path) })
                ?? Found(reason: Self.offline(name, path))
            e.inFlight = false
            let current = entries.contains { $0 === e }
            if self.round === round, round.pending.remove(ObjectIdentifier(e)) != nil {
                if current { round.results[ObjectIdentifier(e)] = found }
                if round.pending.isEmpty { close(round) }
            } else if current {
                apply(e, found)
            }
        }
    }

    /// Applies what came back in configuration order; libraries still out are offline.
    private func close(_ round: Round) {
        if self.round === round { self.round = nil }
        for e in entries {
            if let found = round.results[ObjectIdentifier(e)] {
                apply(e, found)
            } else if round.pending.contains(ObjectIdentifier(e)) {
                let (id, now, repository) = (e.id, clock.now(), self.rows)
                executor.spawn { [executor] in
                    _ = try? await executor.worker { () async throws(BroError) in try await repository.setSeen(id, online: false, at: now) }
                }
                apply(e, Found(reason: Self.offline(e.name, e.path), free: nil, seen: e.seen))
            }
        }
        let waiters = round.waiters
        round.waiters = []
        for w in waiters { w.resume(returning: ()) }
    }

    private func apply(_ e: Entry, _ found: Found) {
        let changed = !e.known || e.online != (found.reason == nil) || e.reason != found.reason
        e.known = true
        e.online = found.reason == nil
        e.reason = found.reason
        e.free = found.free
        if let seen = found.seen { e.seen = seen }
        updateSnapshot()
        guard changed else { return }
        scheduler.setLibraryOffline(e.id, reason: found.reason?.toError())
        bus.publish(.libraryChanged, data: .object([("library", LibraryDto.json(Self.dto(e)))]))
        for code in Self.reasonCodes where code != found.reason?.code { problems.clear(code, scope: Self.scope(e.id)) }
        if let reason = found.reason {
            problems.raise(reason, scope: Self.scope(e.id), actions: [reason.code == .libraryNotAttached ? .attachLibrary : .openSettings])
        }
    }

    private func updateSnapshot() {
        var snapshot: [String: (name: String, path: String, reason: BroMessage?)] = [:]
        for e in entries { snapshot[e.id] = (e.name, e.path, e.known ? e.reason : Self.offline(e.name, e.path)) }
        shared.lock.withLock { shared.snapshot = snapshot }
    }

    private func find(_ id: String) -> Entry? { entries.first { $0.id == id } }

    private static func dto(_ e: Entry) -> LibraryDto {
        LibraryDto(id: e.id, name: e.name, path: e.path, online: e.known && e.online, reason: e.known ? e.reason : nil, freeBytes: e.online ? e.free : nil,
                   lastSeenAt: e.seen)
    }

    private nonisolated static func scope(_ id: String) -> ProblemScope { ProblemScope(kind: .library, id: id) }

    /// The configured library (from the configuration itself, so a roll forward works before `start`).
    private func configured(_ libraryId: String) throws(BroError) -> (id: String, name: String, path: String) {
        guard let w = Config.libraries(config.current()).first(where: { $0.id == libraryId }) else { throw Self.message(.libraryNotFound).toError() }
        return (w.id, w.name.isEmpty ? w.id : w.name, w.path)
    }

    // ---- marker checks (workers) ----------------------------------------------------------------------------------

    /// The marker check of one library; stores what it found.
    private nonisolated func inspect(_ id: String, _ name: String, _ path: String) async -> Found {
        var row: LibraryRecord?
        var reason: BroMessage?
        do throws(BroError) {
            row = try await rows.get(id)
            reason = try await markerReason(id, name, path, row)
        } catch {
            reason = Self.offline(name, path)
        }
        let now = clock.now()
        _ = try? await rows.setSeen(id, online: reason == nil, at: now)
        return reason == nil ? Found(reason: nil, free: Self.freeBytes(fs, path), seen: now) : Found(reason: reason, free: nil, seen: row?.lastSeenAt)
    }

    /// Nil when the folder holds the stored marker (adopting a marker that isn't stored yet); otherwise why the
    /// library is offline.
    private nonisolated func markerReason(_ id: String, _ name: String, _ path: String, _ row: LibraryRecord?) async throws(BroError) -> BroMessage? {
        guard fs.exists(path) else { return Self.offline(name, path) }
        let markerPath = path + "/" + LibraryMarker.fileName
        guard fs.exists(markerPath) else {
            return row == nil ? Self.message(.libraryNotAttached, [("path", .string(path))], severity: .warning) : Self.offline(name, path)
        }
        guard let marker = Self.decode(fs, markerPath) else { return Self.offline(name, path) }
        guard let row, row.path == path else {
            let now = clock.now()
            try await rows.upsert(LibraryRecord(id: id, name: name, path: path, markerId: marker.id, attachedAt: marker.created ?? now, online: true,
                                                     lastSeenAt: now))
            return nil
        }
        if row.markerId != marker.id {
            return Self.message(.libraryMarkerMismatch, [("path", .string(path)), ("found", .string(marker.id)), ("expected", .string(row.markerId))])
        }
        return nil
    }

    private nonisolated func checkMarker(_ lib: (id: String, name: String, path: String)) async throws(BroError) {
        var reason: BroMessage?
        do throws(BroError) {
            reason = try await markerReason(lib.id, lib.name, lib.path, try await rows.get(lib.id))
        } catch {
            reason = Self.offline(lib.name, lib.path)
        }
        if let reason { throw reason.toError() }
    }

    private nonisolated static func decode(_ fs: any FileSystem, _ markerPath: String) -> LibraryMarker? {
        (try? fs.read(markerPath)).flatMap(LibraryMarker.decode)
    }

    private nonisolated static func freeBytes(_ fs: any FileSystem, _ path: String) -> Int64? { try? fs.volume(path).freeBytes }

    // ---- the commit (workers) -------------------------------------------------------------------------------------

    private nonisolated func run(_ unit: UnitRecord, _ intent: CommitIntent, _ files: [UnitFile], tag: String?,
                                 cancel: CancellationToken) async throws(BroError) -> CommitResult {
        try await executor.worker { [self] () async throws(BroError) -> CommitResult in
            var (unit, intent) = (unit, intent)
            let lib = try online(unit.libraryId)
            if tag == nil && !Self.inside(lib.path, intent.destination) { throw Self.message(.libraryNotFound).toError() }
            try await claim(unit.id, cancel: cancel)
            defer { unclaim(unit.id) }
            try await checkMarker(lib)
            if let open = try await openIntent(unit.id) { return try await complete(lib.path, open, files) }
            // Committed already (a roll forward finished it, or the engine stopped before the step's result was stored).
            // Staging then holds none of its items; one that does was made again (a job retried after its commit) and is
            // never passed off as the unit in the library.
            if let done = try await units.get(unit.id), done.state == .committed || done.state == .quarantined {
                if intent.items.contains(where: { fs.exists(intent.staging + "/" + $0.fromPath) }) { throw Self.alreadyExists(lib.path + "/" + done.path) }
                try removeEmpty(intent.staging)
                return CommitResult(unitId: unit.id, destination: lib.path + "/" + done.path, files: files, merged: done.state == .committed && intent.merge)
            }
            if let tag {
                let folder = lib.path + "/" + Self.quarantineFolder
                try ensureFolders(lib.path, folder)
                let name = ConflictNamer.next(Self.lastPart(intent.destination) + " [" + tag + "]", existing: try fs.list(folder).map(\.name), isFolder: true)
                intent.destination = folder + "/" + name
                intent.merge = false
                intent.quarantine = true
                unit.path = Self.quarantineFolder + "/" + name
            }
            try precheck(intent)
            unit.state = .committing
            try await units.beginCommit(unit, intent: intent)
            try crash?("commit.afterIntent")
            return try await complete(lib.path, intent, files)
        }
    }

    /// Nothing is written unless every item is in staging with SHA256SUMS, and nothing at the destination is in the
    /// way.
    private nonisolated func precheck(_ intent: CommitIntent) throws(BroError) {
        guard try isFolder(intent.staging) else { throw Self.notFound(intent.staging) }
        guard fs.exists(intent.staging + "/" + Sha256Sums.fileName) else { throw Self.notFound(intent.staging + "/" + Sha256Sums.fileName) }
        for item in intent.items where !fs.exists(intent.staging + "/" + item.fromPath) { throw Self.notFound(intent.staging + "/" + item.fromPath) }
        guard intent.merge else {
            if fs.exists(intent.destination) { throw Self.message(.libraryFolderExists, [("path", .string(intent.destination))]).toError() }
            return
        }
        for item in intent.items {
            let to = intent.destination + "/" + item.toPath
            if item.isDir {
                try checkMerge(intent.staging + "/" + item.fromPath, to)
            } else if fs.exists(to) {
                throw Self.alreadyExists(to)
            }
        }
    }

    /// Refuses what moveMerging would put under another name than SHA256SUMS lists: it matches names ignoring ASCII case
    /// and renames whatever then clashes, so every entry of that kind is in the way, a folder spelled another way too.
    private nonisolated func checkMerge(_ source: String, _ target: String) throws(BroError) {
        guard fs.exists(target) else { return }
        guard try isFolder(target) else { throw Self.alreadyExists(target) }
        let there = try fs.list(target)
        for e in try fs.list(source).sorted(by: { $0.name.utf8.lexicographicallyPrecedes($1.name.utf8) }) {
            guard let same = there.first(where: { ConflictNamer.same($0.name, e.name) }) else { continue }
            if !e.isDirectory || !same.name.utf8.elementsEqual(e.name.utf8) { throw Self.alreadyExists(target + "/" + same.name) }
            try checkMerge(source + "/" + e.name, target + "/" + e.name)
        }
    }

    /// Steps 5 and 6 from wherever the intent is: the move, SHA256SUMS, the syncs, then the unit committed.
    private nonisolated func complete(_ root: String, _ intent: CommitIntent, _ files: [UnitFile]?) async throws(BroError) -> CommitResult {
        var moved = intent.items.filter(\.moved).count
        let pending = intent.items.filter { !$0.moved }.sorted { $0.seq < $1.seq }
        let stagingThere = try isFolder(intent.staging)
        let whole = !intent.merge && !pending.isEmpty && stagingThere && !fs.exists(intent.destination)
        if whole {
            try ensureFolders(root, parentFolder(of: intent.destination))
            try fs.rename(intent.staging, to: intent.destination)
        }
        for item in pending {
            if !whole { try move(root, intent, item) }
            try await units.markMoved(intent.unitId, seq: item.seq)
            moved += 1
            try crash?("commit.afterMove:\(moved)")
        }
        let stagingSums = intent.staging + "/" + Sha256Sums.fileName
        let sums = intent.destination + "/" + Sha256Sums.fileName
        if fs.exists(stagingSums) {
            try ensureFolders(root, intent.destination)
            if intent.merge || fs.exists(sums) {
                let existing = fs.exists(sums) ? try fs.read(sums) : []
                let merged = Sha256Sums.merge(existing, entries: Sha256Sums.parse(String(decoding: try fs.read(stagingSums), as: UTF8.self)))
                try fs.writeAtomically(sums, bytes: merged, mode: Self.fileMode)
                try fs.remove(stagingSums)
            } else {
                try fs.rename(stagingSums, to: sums)
            }
        }
        if intent.merge { try ignored(intent) }
        try removeEmpty(intent.staging)
        var unitFiles = files ?? []
        if files == nil { unitFiles = try fromSums(intent) }
        // Whatever happened on the way, a unit never ends committed naming a file that isn't there: the intent stays open.
        for f in unitFiles where !fs.exists(intent.destination + "/" + f.path) { throw Self.notFound(intent.destination + "/" + f.path) }
        for folder in Self.synced(root, intent, unitFiles) { try fs.syncDirectory(folder) }
        try crash?("commit.beforeDone")
        guard var record = try await units.get(intent.unitId) else { throw Self.message(.unitNotFound).toError() }
        record.state = intent.quarantine ? .quarantined : .committed
        record.committedAt = clock.now()
        try await units.finishCommit(intent, record: record, files: unitFiles)
        return CommitResult(unitId: intent.unitId, destination: intent.destination, files: unitFiles, merged: intent.merge)
    }

    /// One item: a file renamed, a folder merged; one already at its destination is recognised by its hash, never
    /// replaced.
    private nonisolated func move(_ root: String, _ intent: CommitIntent, _ item: CommitItem) throws(BroError) {
        let from = intent.staging + "/" + item.fromPath
        let to = intent.destination + "/" + item.toPath
        let (there, arrived) = (fs.exists(from), fs.exists(to))
        if item.isDir {
            if there {
                try ensureFolders(root, parentFolder(of: to))
                _ = try fs.moveMerging(from, to: to, policy: .neverReplace)
            } else if !arrived {
                throw Self.notFound(from)
            }
            return
        }
        if there && !arrived {
            try ensureFolders(root, parentFolder(of: to))
            try fs.rename(from, to: to)
        } else if !there && arrived {
            if let sha = item.sha256, try hash(to) != sha { throw Self.alreadyExists(to) }
        } else if there {
            throw Self.alreadyExists(to)
        } else {
            throw Self.notFound(from)
        }
    }

    /// A media server's Backup/ folder the unit was merged into gets .plexignore and .ignore when they are missing:
    /// outside the unit (not in SHA256SUMS), never overwritten, each written atomically. Told from the intent alone (an
    /// item under Backup/ of a shared folder), so a roll forward writes them too.
    private nonisolated func ignored(_ intent: CommitIntent) throws(BroError) {
        guard intent.items.contains(where: { Self.starts($0.toPath, Self.backupFolder + "/") }) else { return }
        let folder = intent.destination + "/" + Self.backupFolder
        guard try isFolder(folder) else { return }
        for (name, text) in Self.ignoreFiles where !fs.exists(folder + "/" + name) {
            try fs.writeAtomically(folder + "/" + name, bytes: Array(text.utf8), mode: Self.fileMode)
        }
    }

    /// The unit's lines of the destination's SHA256SUMS (a roll forward has no file list of its own).
    private nonisolated func fromSums(_ intent: CommitIntent) throws(BroError) -> [UnitFile] {
        let sums = Sha256Sums.parse(String(decoding: try fs.read(intent.destination + "/" + Sha256Sums.fileName), as: UTF8.self))
        let recorded = try recordedFiles(intent)
        var files: [UnitFile] = []
        for s in sums where intent.items.contains(where: { $0.isDir ? Self.starts(s.path, $0.toPath + "/") : Self.same(s.path, $0.toPath) }) {
            let file = recorded[Array(s.path.utf8)]
            files.append(UnitFile(path: s.path, size: try fs.stat(intent.destination + "/" + s.path).size, sha256: s.sha256,
                                  role: file?["role"]?.string ?? "", title: file?["title"]?.int.map { Int($0) }))
        }
        return files
    }

    /// The files of the unit's archive record, by path; none when it isn't there or doesn't read.
    private nonisolated func recordedFiles(_ intent: CommitIntent) throws(BroError) -> [[UInt8]: JsonValue] {
        let record = intent.destination + "/bromelia-" + String(intent.unitId.value.prefix(8)) + ".json"
        guard fs.exists(record) else { return [:] }
        var files: [[UInt8]: JsonValue] = [:]
        for file in JsonValue.parse(try fs.read(record))?["files"]?.array ?? [] {
            if let path = file["path"]?.string { files[Array(path.utf8)] = file }
        }
        return files
    }

    /// Every folder from the library's root down to each committed file, and the staging folder's parent.
    private nonisolated static func synced(_ root: String, _ intent: CommitIntent, _ files: [UnitFile]) -> [String] {
        var folders: Set<[UInt8]> = [Array(parentFolder(of: intent.staging).utf8)]
        for f in files {
            var folder = root
            folders.insert(Array(folder.utf8))
            for part in below(root, intent.destination + "/" + f.path).dropLast() {
                folder += "/" + part
                folders.insert(Array(folder.utf8))
            }
        }
        return folders.sorted { $0.lexicographicallyPrecedes($1) }.map { String(decoding: $0, as: UTF8.self) }
    }

    /// Makes the folders from below the library's root down to `folder`; never the root.
    private nonisolated func ensureFolders(_ root: String, _ folder: String) throws(BroError) {
        if Self.same(folder, root) { return }
        guard Self.inside(root, folder) else { throw Self.message(.libraryNotFound).toError() }
        var current = root
        for part in Self.below(root, folder) {
            current += "/" + part
            if !fs.exists(current) { try fs.createDirectory(current, parentsMustExist: true) }
        }
    }

    /// Removes the staging folder's empty folders, and the folder itself once empty; anything else stays.
    private nonisolated func removeEmpty(_ folder: String) throws(BroError) {
        guard try isFolder(folder) else { return }
        for e in try fs.list(folder) where e.isDirectory { try removeEmpty(folder + "/" + e.name) }
        if try fs.list(folder).isEmpty { try fs.remove(folder) }
    }

    private nonisolated func walk(_ folder: String) throws(BroError) -> [String] {
        var out: [String] = []
        for e in try fs.list(folder).sorted(by: { $0.name.utf8.lexicographicallyPrecedes($1.name.utf8) }) {
            if e.isDirectory { out += try walk(folder + "/" + e.name).map { e.name + "/" + $0 } } else { out.append(e.name) }
        }
        return out
    }

    private nonisolated func hash(_ path: String) throws(BroError) -> String {
        let stream = try fs.openForReading(path, bypassCache: true)
        defer { stream.close() }
        let hasher = Sha256Hasher()
        while true {
            let chunk = try stream.read(1 << 20)
            if chunk.isEmpty { break }
            hasher.update(chunk)
        }
        return hasher.finish()
    }

    private nonisolated func isFolder(_ path: String) throws(BroError) -> Bool { fs.exists(path) ? try fs.stat(path).isDirectory : false }

    private nonisolated func openIntent(_ unitId: Id) async throws(BroError) -> CommitIntent? {
        try await units.openIntents().first { $0.unitId == unitId }
    }

    /// Holds the unit for one commit or roll forward, first waiting while another one has it; job.cancelled when `cancel`
    /// fires while it waits (once held, a commit runs to its end or its intent stays). The wait is inside a worker, so the
    /// executor's activity counts it.
    private nonisolated func claim(_ unitId: Id, cancel: CancellationToken?) async throws(BroError) {
        while true {
            // 0: held; -1: cancelled; else the waiter's number.
            let waiter: Int = shared.lock.withLock {
                if cancel?.isCancelled == true { return -1 }
                if shared.committing[unitId] == nil {
                    shared.committing[unitId] = []
                    return 0
                }
                shared.waiting += 1
                shared.nextWaiter += 1
                return shared.nextWaiter
            }
            if waiter < 0 { throw BroError(MessageCode.wire(.jobCancelled)) }
            if waiter == 0 { return }
            var stopListening: (@Sendable () -> Void)?
            await withCheckedContinuation { (wake: CheckedContinuation<Void, Never>) in
                let queued = shared.lock.withLock { () -> Bool in
                    guard shared.committing[unitId] != nil else { return false } // freed meanwhile: try again
                    shared.committing[unitId]!.append((waiter, wake))
                    return true
                }
                if !queued { wake.resume(); return }
                stopListening = cancel?.onCancel { [self] in self.wake(unitId, waiter) }
            }
            stopListening?()
            shared.lock.withLock { shared.waiting -= 1 }
        }
    }

    /// Wakes one waiter of the unit (its cancel), unless the holder's end woke it already.
    private nonisolated func wake(_ unitId: Id, _ waiter: Int) {
        let found = shared.lock.withLock { () -> CheckedContinuation<Void, Never>? in
            guard let i = shared.committing[unitId]?.firstIndex(where: { $0.id == waiter }) else { return nil }
            return shared.committing[unitId]!.remove(at: i).wake
        }
        found?.resume()
    }

    private nonisolated func unclaim(_ unitId: Id) {
        let waiters = shared.lock.withLock { shared.committing.removeValue(forKey: unitId) ?? [] }
        for w in waiters { w.wake.resume() }
    }

    /// Strictly below the root, and not in its .bromelia folder.
    private nonisolated static func inside(_ root: String, _ path: String) -> Bool {
        starts(path, root + "/") && !same(path, root + "/" + staging) && !starts(path, root + "/" + staging + "/")
    }

    // Paths by their bytes: Swift's String compares and splits by grapheme, so a '/' a combining mark follows wouldn't
    // split a path, and canonically equal names would be one.

    private nonisolated static func starts(_ path: String, _ prefix: String) -> Bool { path.utf8.starts(with: prefix.utf8) }

    private nonisolated static func same(_ a: String, _ b: String) -> Bool { a.utf8.elementsEqual(b.utf8) }

    /// The parts of `path` below `root` (`path` is inside it), split at each '/'.
    private nonisolated static func below(_ root: String, _ path: String) -> [String] {
        path.utf8.dropFirst(root.utf8.count + 1).split(separator: slash, omittingEmptySubsequences: false).map { String(decoding: $0, as: UTF8.self) }
    }

    private nonisolated static func lastPart(_ path: String) -> String {
        var p = Array(path.utf8)
        while p.last == slash { p.removeLast() }
        return String(decoding: p.lastIndex(of: slash).map { p[($0 + 1)...] } ?? p[...], as: UTF8.self)
    }

    private nonisolated static func offline(_ name: String, _ path: String) -> BroMessage {
        message(.libraryOffline, [("library", .string(name)), ("path", .string(path))], severity: .warning)
    }

    private nonisolated static func message(_ code: MessageCode, _ params: [(key: String, value: JsonValue)] = [], severity: Severity = .error) -> BroMessage {
        BroMessage(code, params, severity: severity)
    }

    private nonisolated static func notFound(_ path: String) -> BroError { message(.fsNotFound, [("path", .string(path))]).toError() }

    private nonisolated static func alreadyExists(_ path: String) -> BroError { message(.fsAlreadyExists, [("path", .string(path))]).toError() }
}
