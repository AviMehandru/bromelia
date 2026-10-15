import BroDomain
import BroFoundation
import BroPorts
import Foundation

/// The libraries of the configuration (plan §11.1, §22). A library is online while its folder holds the
/// .bromelia-library marker it was attached with (I2): the markers are checked at start, every 30 s and on `check()`
/// (mount events), each on its own worker; an offline library is blocked in the scheduler and raised as a problem. A
/// check that hasn't come back by the next tick (a share that froze, review L3-9) counts as offline and isn't started
/// again until it comes back. As the `LibraryPort` of the steps it gives staging folders and runs the commit protocol
/// (§22.3 steps 4–6) and its roll forward (§15.8) on workers. State is touched only on the executor; the steps read a
/// snapshot. shared/fixtures/engine/library-service.cases.json.
@EngineActor
public final class LibraryService: LibraryPort {
    private nonisolated static let fileMode = 0o644
    private nonisolated static let staging = ".bromelia"
    private nonisolated static let quarantineFolder = "_Quarantine"
    private nonisolated static let incompleteNote = "INCOMPLETE.txt"
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

    /// The snapshot the steps read, and the units being committed.
    private final class Shared: @unchecked Sendable {
        let lock = NSLock()
        var snapshot: [String: (name: String, path: String, reason: BroMessage?)] = [:]
        var committing: Set<Id> = []
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
                crash: (@Sendable (String) throws(BroError) -> Void)? = nil) {
        self.executor = executor
        self.bus = bus
        self.problems = problems
        self.scheduler = scheduler
        self.config = config
        self.clock = clock
        self.fs = fs
        self.rows = libraries
        self.units = units
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

    public nonisolated func commit(_ unit: UnitRecord, intent: CommitIntent, files: [UnitFile]) async throws(BroError) -> CommitResult {
        try await run(unit, intent, files, tag: nil)
    }

    public nonisolated func quarantine(_ unit: UnitRecord, intent: CommitIntent, files: [UnitFile], tag: String) async throws(BroError) -> CommitResult {
        try await run(unit, intent, files, tag: tag)
    }

    /// Finishes an interrupted commit (§15.8): the marker checked, then what is left moved (a file already at its
    /// destination counts when its SHA-256 is the item's), SHA256SUMS merged, the folders synced and the unit committed,
    /// its files read from the destination's SHA256SUMS. Off the executor.
    public nonisolated func rollForward(_ intent: CommitIntent) async throws(BroError) -> CommitResult {
        let units = self.units
        guard let unit = try await units.get(intent.unitId) else { throw Self.message(.unitNotFound).toError() }
        let lib = try await configured(unit.libraryId)
        return try await executor.worker { [self] () async throws(BroError) -> CommitResult in
            try await checkMarker(lib)
            try claim(intent.unitId)
            defer { unclaim(intent.unitId) }
            return try await complete(lib.path, intent, nil)
        }
    }

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
    /// `note` as INCOMPLETE.txt (written first, so the folder arrives marked: I1), never reused or deleted (review L3-3,
    /// P1-3); one that holds no file is removed instead (nil). The folders are synced. Any thread but the executor.
    public nonisolated func setAside(_ libraryId: String, folder: String, name: String, note: [UInt8]) async throws(BroError) -> String? {
        let lib = try await configured(libraryId)
        return try await executor.worker { [self] () async throws(BroError) -> String? in
            try await checkMarker(lib)
            let staging = lib.path + "/" + Self.staging + "/staging/" + folder
            guard try isFolder(staging) else { throw Self.notFound(staging) }
            if try walk(staging).isEmpty {
                try removeEmpty(staging)
                return nil
            }
            try fs.writeAtomically(staging + "/" + Self.incompleteNote, bytes: note, mode: Self.fileMode)
            let quarantine = lib.path + "/" + Self.quarantineFolder
            try ensureFolders(lib.path, quarantine)
            let dest = quarantine + "/" + ConflictNamer.next(name + " [INCOMPLETE]", existing: try fs.list(quarantine).map(\.name), isFolder: true)
            try fs.rename(staging, to: dest)
            try fs.syncDirectory(quarantine)
            try fs.syncDirectory(Self.parent(staging))
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

    private nonisolated func run(_ unit: UnitRecord, _ intent: CommitIntent, _ files: [UnitFile], tag: String?) async throws(BroError) -> CommitResult {
        try await executor.worker { [self] () async throws(BroError) -> CommitResult in
            var (unit, intent) = (unit, intent)
            let lib = try online(unit.libraryId)
            if tag == nil && !Self.inside(lib.path, intent.destination) { throw Self.message(.libraryNotFound).toError() }
            try await checkMarker(lib)
            try claim(unit.id)
            defer { unclaim(unit.id) }
            if let open = try await units.openIntents().first(where: { $0.unitId == unit.id }) {
                return try await complete(lib.path, open, files)
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
    /// way (I5).
    private nonisolated func precheck(_ intent: CommitIntent) throws(BroError) {
        guard try isFolder(intent.staging) else { throw Self.notFound(intent.staging) }
        guard fs.exists(intent.staging + "/" + Sha256Sums.fileName) else { throw Self.notFound(intent.staging + "/" + Sha256Sums.fileName) }
        for item in intent.items where !fs.exists(intent.staging + "/" + item.fromPath) { throw Self.notFound(intent.staging + "/" + item.fromPath) }
        guard intent.merge else {
            if fs.exists(intent.destination) { throw Self.message(.libraryFolderExists, [("path", .string(intent.destination))]).toError() }
            return
        }
        for item in intent.items {
            var targets = [intent.destination + "/" + item.toPath]
            if item.isDir { targets = try walk(intent.staging + "/" + item.fromPath).map { intent.destination + "/" + item.toPath + "/" + $0 } }
            for t in targets where fs.exists(t) {
                if !(try fs.stat(t).isDirectory) { throw Self.alreadyExists(t) }
            }
        }
    }

    /// Steps 5 and 6 from wherever the intent is: the move, SHA256SUMS, the syncs, then the unit committed.
    private nonisolated func complete(_ root: String, _ intent: CommitIntent, _ files: [UnitFile]?) async throws(BroError) -> CommitResult {
        var moved = intent.items.filter(\.moved).count
        let pending = intent.items.filter { !$0.moved }.sorted { $0.seq < $1.seq }
        let stagingThere = try isFolder(intent.staging)
        let whole = !intent.merge && !pending.isEmpty && stagingThere && !fs.exists(intent.destination)
        if whole {
            try ensureFolders(root, Self.parent(intent.destination))
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
                let existing = fs.exists(sums) ? String(decoding: try fs.read(sums), as: UTF8.self) : ""
                let merged = Sha256Sums.merge(existing, entries: Sha256Sums.parse(String(decoding: try fs.read(stagingSums), as: UTF8.self)))
                try fs.writeAtomically(sums, bytes: Array(merged.utf8), mode: Self.fileMode)
                try fs.remove(stagingSums)
            } else {
                try fs.rename(stagingSums, to: sums)
            }
        }
        try removeEmpty(intent.staging)
        var unitFiles = files ?? []
        if files == nil { unitFiles = try fromSums(intent) }
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
                try ensureFolders(root, Self.parent(to))
                _ = try fs.moveMerging(from, to: to, policy: .neverReplace)
            } else if !arrived {
                throw Self.notFound(from)
            }
            return
        }
        if there && !arrived {
            try ensureFolders(root, Self.parent(to))
            try fs.rename(from, to: to)
        } else if !there && arrived {
            if let sha = item.sha256, try hash(to) != sha { throw Self.alreadyExists(to) }
        } else if there {
            throw Self.alreadyExists(to)
        } else {
            throw Self.notFound(from)
        }
    }

    /// The unit's lines of the destination's SHA256SUMS (a roll forward has no file list of its own).
    private nonisolated func fromSums(_ intent: CommitIntent) throws(BroError) -> [UnitFile] {
        let sums = Sha256Sums.parse(String(decoding: try fs.read(intent.destination + "/" + Sha256Sums.fileName), as: UTF8.self))
        var files: [UnitFile] = []
        for s in sums where intent.items.contains(where: { $0.isDir ? s.path.hasPrefix($0.toPath + "/") : s.path == $0.toPath }) {
            files.append(UnitFile(path: s.path, size: try fs.stat(intent.destination + "/" + s.path).size, sha256: s.sha256, role: "title"))
        }
        return files
    }

    /// Every folder from the library's root down to each committed file, and the staging folder's parent.
    private nonisolated static func synced(_ root: String, _ intent: CommitIntent, _ files: [UnitFile]) -> [String] {
        var folders: Set<String> = [parent(intent.staging)]
        for f in files {
            var folder = root
            folders.insert(folder)
            let parts = String((intent.destination + "/" + f.path).dropFirst(root.count + 1)).split(separator: "/", omittingEmptySubsequences: false)
            for part in parts.dropLast() {
                folder += "/" + part
                folders.insert(folder)
            }
        }
        return folders.sorted { $0.utf8.lexicographicallyPrecedes($1.utf8) }
    }

    /// Makes the folders from below the library's root down to `folder`; never the root.
    private nonisolated func ensureFolders(_ root: String, _ folder: String) throws(BroError) {
        if folder == root { return }
        guard Self.inside(root, folder) else { throw Self.message(.libraryNotFound).toError() }
        var current = root
        for part in String(folder.dropFirst(root.count + 1)).split(separator: "/", omittingEmptySubsequences: false) {
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

    private nonisolated func claim(_ unitId: Id) throws(BroError) {
        let claimed = shared.lock.withLock { shared.committing.insert(unitId).inserted }
        if !claimed { throw Self.message(.commitInProgress, [("unit", .string(unitId.value))]).toError() }
    }

    private nonisolated func unclaim(_ unitId: Id) { _ = shared.lock.withLock { shared.committing.remove(unitId) } }

    /// Strictly below the root, and not in its .bromelia folder.
    private nonisolated static func inside(_ root: String, _ path: String) -> Bool {
        path.hasPrefix(root + "/") && path != root + "/" + staging && !path.hasPrefix(root + "/" + staging + "/")
    }

    private nonisolated static func parent(_ path: String) -> String { String(path[..<path.lastIndex(of: "/")!]) }

    private nonisolated static func lastPart(_ path: String) -> String {
        var p = path
        while p.hasSuffix("/") { p.removeLast() }
        return p.lastIndex(of: "/").map { String(p[p.index(after: $0)...]) } ?? p
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
