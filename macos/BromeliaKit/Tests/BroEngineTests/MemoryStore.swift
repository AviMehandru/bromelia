import BroDomain
import BroEngine
import BroFoundation
import BroPorts
import Foundation

/// Jobs and steps in memory (the rest of the store isn't needed by these tests). Thread-safe.
final class MemoryStore: Store, JobRepository, StepRepository, @unchecked Sendable {
    private let lock = NSLock()
    private var jobsById: [Id: JobRecord] = [:]
    private var order: [Id] = [] // jobs in the order they were inserted
    private var stepsByKey: [String: StepRecord] = [:]

    func jobs() -> any JobRepository { self }
    func steps() -> any StepRepository { self }
    func units() -> any UnitRepository { fatalError("not used") }
    func catalog() -> any CatalogRepository { fatalError("not used") }
    func checks() -> any CheckRepository { fatalError("not used") }
    func replicas() -> any ReplicaRepository { fatalError("not used") }
    func drives() -> any DriveRepository { fatalError("not used") }
    func libraries() -> any LibraryRepository { fatalError("not used") }
    func lookupCache() -> any LookupCacheRepository { fatalError("not used") }
    func outbox() -> any OutboxRepository { fatalError("not used") }
    func kv() -> any KeyValueRepository { fatalError("not used") }
    func transaction<T: Sendable>(_ block: @Sendable (any Store) async throws(BroError) -> T) async throws(BroError) -> T { try await block(self) }
    func migrate() async throws(BroError) {}

    private var holding = false
    private var held: [(job: JobRecord, done: CheckedContinuation<Void, Never>)] = []

    /// From now on job inserts wait until `releaseInserts()` (a test's way to act while one is written).
    func holdInserts() { lock.withLock { holding = true } }

    func releaseInserts() {
        let ready = lock.withLock { () -> [CheckedContinuation<Void, Never>] in
            holding = false
            for (job, _) in held { put(job) }
            defer { held.removeAll() }
            return held.map(\.done)
        }
        ready.forEach { $0.resume() }
    }

    /// Inserts waiting for `releaseInserts()`.
    var heldInserts: Int { lock.withLock { held.count } }

    private var holdLoadsOf: Set<Id> = []
    private var heldLoadList: [CheckedContinuation<Void, Never>] = []

    /// From now on loads of the job's record wait until `releaseLoads()` (a run that is slow to load).
    func holdLoads(_ jobId: Id) { _ = lock.withLock { holdLoadsOf.insert(jobId) } }

    func releaseLoads() {
        let ready = lock.withLock { () -> [CheckedContinuation<Void, Never>] in
            holdLoadsOf.removeAll()
            defer { heldLoadList.removeAll() }
            return heldLoadList
        }
        ready.forEach { $0.resume() }
    }

    /// Loads waiting for `releaseLoads()`.
    var heldLoads: Int { lock.withLock { heldLoadList.count } }

    /// The lock held.
    private func put(_ job: JobRecord) {
        if jobsById[job.id] == nil { order.append(job.id) }
        jobsById[job.id] = job
    }

    func insert(_ job: JobRecord) async throws(BroError) {
        await withCheckedContinuation { (done: CheckedContinuation<Void, Never>) in
            let now = lock.withLock { () -> Bool in
                if holding {
                    held.append((job, done))
                    return false
                }
                put(job)
                return true
            }
            if now { done.resume() }
        }
    }
    func update(_ job: JobRecord) async throws(BroError) { lock.withLock { jobsById[job.id] = job } }
    func load(_ jobId: Id) async throws(BroError) -> JobRecord? {
        await withCheckedContinuation { (done: CheckedContinuation<Void, Never>) in
            let now = lock.withLock { () -> Bool in
                guard holdLoadsOf.contains(jobId) else { return true }
                heldLoadList.append(done)
                return false
            }
            if now { done.resume() }
        }
        return lock.withLock { jobsById[jobId] }
    }
    func active() async throws(BroError) -> [JobRecord] {
        lock.withLock {
            let rank: [Queue: Int] = [.acquisition: 0, .processing: 1, .maintenance: 2]
            // Stable: insertion order breaks the remaining ties, like created_at in SqliteStore.
            return order.compactMap { jobsById[$0] }.filter { $0.state != .finished }.enumerated()
                .sorted { ($0.element.queue != $1.element.queue) ? rank[$0.element.queue]! < rank[$1.element.queue]!
                    : $0.element.position != $1.element.position ? $0.element.position < $1.element.position : $0.offset < $1.offset }
                .map(\.element)
        }
    }
    func query(_ query: JobQuery) async throws(BroError) -> [JobRecord] {
        lock.withLock { Array(matching(query).dropFirst(query.offset).prefix(query.limit)) }
    }
    func count(_ query: JobQuery) async throws(BroError) -> Int { lock.withLock { matching(query).count } }

    /// What SqliteStore's query matches, newest first (the lock held).
    private func matching(_ q: JobQuery) -> [JobRecord] {
        order.compactMap { jobsById[$0] }.filter { j in
            (q.states.isEmpty || q.states.contains(j.state)) && (q.outcomes.isEmpty || j.outcome.map(q.outcomes.contains) == true)
                && (q.kind == nil || j.kind == q.kind) && (q.queue == nil || j.queue == q.queue) && (q.driveId == nil || j.driveId == q.driveId)
                && (q.unitId == nil || j.unitId == q.unitId)
                && (q.finishedAfter == nil || (j.finishedAt.map { $0.unixMilliseconds > q.finishedAfter!.unixMilliseconds } ?? false))
        }.sorted { $0.createdAt.unixMilliseconds != $1.createdAt.unixMilliseconds ? $0.createdAt.unixMilliseconds > $1.createdAt.unixMilliseconds
            : $0.id.value > $1.id.value }
    }

    func save(_ step: StepRecord) async throws(BroError) { lock.withLock { stepsByKey["\(step.jobId.value)/\(step.seq)"] = step } }
    func completed(_ jobId: Id) async throws(BroError) -> [StepRecord] {
        lock.withLock { stepsByKey.values.filter { $0.jobId == jobId && $0.state == .succeeded }.sorted { $0.seq < $1.seq } }
    }
    func load(_ jobId: Id) async throws(BroError) -> [StepRecord] {
        lock.withLock { stepsByKey.values.filter { $0.jobId == jobId }.sorted { $0.seq < $1.seq } }
    }

    func job(_ id: Id) -> JobRecord? { lock.withLock { jobsById[id] } }
    func stepsOf(_ id: Id) -> [StepRecord] { lock.withLock { stepsByKey.values.filter { $0.jobId == id }.sorted { $0.seq < $1.seq } } }
}

/// Files and folders in memory: what the engine's tests need of the FileSystem port. Thread-safe. While `recording` is on,
/// the calls that change something are written to `ops` ('write', 'sync', 'remove', 'rename', 'syncDirectory', and
/// 'open <path> disk|cache'); `put` and `putFolder` (a tool writing, a case's setup) aren't. `setCached` gives what a read
/// through the cache gives instead of what is on disk; `fail` makes an operation on a path fail before it acts (open,
/// read, sync, syncDirectory, write, remove, rename: its from path); a rename refuses a path that exists and a folder that
/// isn't there, as the real ones do; `onOp` hears of each operation before it acts. Paths are keyed by their
/// UTF-8 bytes, as a disk names them: an NFC and an NFD spelling of one name are two files (Swift's `==` on String
/// would make them one).
final class MemoryFileSystem: FileSystem, @unchecked Sendable {
    private let lock = NSLock()
    private var files: [[UInt8]: [UInt8]] = [:]
    private var folders: Set<[UInt8]> = [Array("/".utf8)]
    private var volumes: [String: VolumeInfo] = [:]
    private var _ops: [String] = []
    private var _recording = false
    private var cached: [[UInt8]: [UInt8]] = [:]
    private var fails: [(op: String, path: String, error: BroError)] = []
    private var _onOp: (@Sendable (String, String) -> Void)?
    private var _reads: [String: Int] = [:]
    private var _openStreams = 0

    var ops: [String] { lock.withLock { _ops } }
    /// How many pieces of each file were read while `recording` was on.
    var reads: [String: Int] { lock.withLock { _reads } }
    /// The streams `openForReading` gave that aren't closed yet.
    var openStreams: Int { lock.withLock { _openStreams } }
    var onOp: (@Sendable (String, String) -> Void)? {
        get { lock.withLock { _onOp } }
        set { lock.withLock { _onOp = newValue } }
    }
    var recording: Bool {
        get { lock.withLock { _recording } }
        set { lock.withLock { _recording = newValue } }
    }

    /// What a read that doesn't bypass the cache gives for this path.
    func setCached(_ path: String, _ bytes: [UInt8]) { lock.withLock { cached[Array(path.utf8)] = bytes } }

    /// Makes `op` on `path` fail with `error`, before it acts.
    func fail(_ op: String, _ path: String, _ error: BroError) { lock.withLock { fails.append((op, path, error)) } }

    /// Forgets the failures `fail` set.
    func clearFails() { lock.withLock { fails.removeAll() } }

    /// Forgets the recorded ops and reads.
    func clearRecorded() {
        lock.withLock {
            _ops.removeAll()
            _reads.removeAll()
        }
    }

    /// `onOp`, then the failure set for this operation and path, if any.
    private func check(_ op: String, _ path: String) throws(BroError) {
        let (hook, error) = lock.withLock { (_onOp, fails.first { $0.op == op && $0.path == path }?.error) }
        hook?(op, path)
        if let error { throw error }
    }

    /// What `volume` gives for this path.
    func setVolume(_ path: String, _ info: VolumeInfo) { lock.withLock { volumes[path] = info } }

    private static func key(_ path: String) -> [UInt8] { Array(path.utf8) }

    private static func notFound(_ path: String) -> BroError { BroError("fs.notFound", [("path", .string(path))]) }

    /// Paths split at the byte '/', as the real file systems do (not at Swift's grapheme '/').
    private static func parent(_ path: String) -> String {
        guard let i = path.utf8.lastIndex(of: UInt8(ascii: "/")), i != path.utf8.startIndex else { return "/" }
        return String(decoding: path.utf8[..<i], as: UTF8.self)
    }

    /// The lock held.
    private func record(_ op: String) { if _recording { _ops.append(op) } }

    /// The lock held.
    private func putFolderLocked(_ path: String) {
        var current = ""
        for part in path.utf8.split(separator: UInt8(ascii: "/")) {
            current += "/" + String(decoding: part, as: UTF8.self)
            folders.insert(MemoryFileSystem.key(current))
        }
    }

    /// A file with these bytes, its folders made.
    func put(_ path: String, _ bytes: [UInt8]) {
        lock.withLock {
            putFolderLocked(MemoryFileSystem.parent(path))
            files[MemoryFileSystem.key(path)] = bytes
        }
    }

    func putFolder(_ path: String) { lock.withLock { putFolderLocked(path) } }

    func exists(_ path: String) -> Bool { lock.withLock { files[MemoryFileSystem.key(path)] != nil || folders.contains(MemoryFileSystem.key(path)) } }
    func read(_ path: String) throws(BroError) -> [UInt8] {
        guard let b = lock.withLock({ files[MemoryFileSystem.key(path)] }) else { throw MemoryFileSystem.notFound(path) }
        return b
    }
    func createDirectory(_ path: String, parentsMustExist: Bool) throws(BroError) { putFolder(path) }
    func writeAtomically(_ path: String, bytes: [UInt8], mode: Int) throws(BroError) {
        try check("write", path)
        let folder = MemoryFileSystem.parent(path)
        let ok = lock.withLock { () -> Bool in
            guard folders.contains(MemoryFileSystem.key(folder)) else { return false }
            files[MemoryFileSystem.key(path)] = bytes
            record("write " + path)
            return true
        }
        if !ok { throw MemoryFileSystem.notFound(folder) }
    }
    func stat(_ path: String) throws(BroError) -> FileInfo {
        let info = lock.withLock { () -> FileInfo? in
            if let b = files[MemoryFileSystem.key(path)] { return FileInfo(size: Int64(b.count), isDirectory: false, modifiedAt: Instant(unixMilliseconds: 0)) }
            return folders.contains(MemoryFileSystem.key(path)) ? FileInfo(size: 0, isDirectory: true, modifiedAt: Instant(unixMilliseconds: 0)) : nil
        }
        guard let info else { throw MemoryFileSystem.notFound(path) }
        return info
    }
    func list(_ directory: String) throws(BroError) -> [DirectoryEntry] {
        let entries = lock.withLock { () -> [DirectoryEntry]? in
            guard folders.contains(MemoryFileSystem.key(directory)) else { return nil }
            let prefix = MemoryFileSystem.key(directory == "/" ? "/" : directory + "/")
            func child(_ p: [UInt8]) -> String? {
                guard p.starts(with: prefix), p.count > prefix.count else { return nil }
                let rest = p.dropFirst(prefix.count)
                return rest.contains(UInt8(ascii: "/")) ? nil : String(decoding: rest, as: UTF8.self)
            }
            return (files.keys.compactMap(child).map { DirectoryEntry(name: $0, isDirectory: false) }
                + folders.compactMap(child).map { DirectoryEntry(name: $0, isDirectory: true) })
                .sorted { $0.name.utf8.lexicographicallyPrecedes($1.name.utf8) }
        }
        guard let entries else { throw MemoryFileSystem.notFound(directory) }
        return entries
    }
    func readRange(_ path: String, offset: Int64, length: Int) throws(BroError) -> [UInt8] {
        let bytes = try read(path)
        guard offset < Int64(bytes.count) else { return [] }
        return Array(bytes[Int(offset)..<min(bytes.count, Int(offset) + length)])
    }
    func rename(_ from: String, to: String) throws(BroError) {
        try check("rename", from)
        let error = lock.withLock { () -> BroError? in
            let from8 = MemoryFileSystem.key(from), to8 = MemoryFileSystem.key(to), inside = MemoryFileSystem.key(from + "/")
            if files[to8] != nil || folders.contains(to8) { return BroError("fs.alreadyExists", [("path", .string(to))]) }
            let parent = MemoryFileSystem.parent(to)
            if !folders.contains(MemoryFileSystem.key(parent)) { return BroError("fs.parentMissing", [("path", .string(parent))]) }
            if let b = files.removeValue(forKey: from8) {
                files[to8] = b
            } else if folders.contains(from8) {
                for f in files.keys where f.starts(with: inside) {
                    files[to8 + f.dropFirst(from8.count)] = files.removeValue(forKey: f)
                }
                for d in folders where d == from8 || d.starts(with: inside) {
                    folders.remove(d)
                    folders.insert(to8 + d.dropFirst(from8.count))
                }
            } else {
                return MemoryFileSystem.notFound(from)
            }
            record("rename \(from) \(to)")
            return nil
        }
        if let error { throw error }
    }
    func moveMerging(_ from: String, to: String, policy: MovePolicy, onMoved: (MovedItem) -> Void) throws(BroError) -> [MovedItem] { fatalError("not used") }
    func remove(_ path: String) throws(BroError) {
        try check("remove", path)
        let error = lock.withLock { () -> BroError? in
            let path8 = MemoryFileSystem.key(path), inside = MemoryFileSystem.key(path + "/")
            if files.removeValue(forKey: path8) != nil {
                record("remove " + path)
                return nil
            }
            guard folders.contains(path8) else { return MemoryFileSystem.notFound(path) }
            if files.keys.contains(where: { $0.starts(with: inside) }) || folders.contains(where: { $0.starts(with: inside) }) {
                return BroError("fs.failed", [("path", .string(path)), ("reason", .string("not empty"))])
            }
            folders.remove(path8)
            record("remove " + path)
            return nil
        }
        if let error { throw error }
    }
    func moveToTrash(_ path: String, trash: String) throws(BroError) { fatalError("not used") }
    func syncFile(_ path: String) throws(BroError) {
        try check("sync", path)
        let ok = lock.withLock { () -> Bool in
            guard files[MemoryFileSystem.key(path)] != nil else { return false }
            record("sync " + path)
            return true
        }
        if !ok { throw MemoryFileSystem.notFound(path) }
    }
    func syncDirectory(_ path: String) throws(BroError) {
        try check("syncDirectory", path)
        let ok = lock.withLock { () -> Bool in
            guard folders.contains(MemoryFileSystem.key(path)) else { return false }
            record("syncDirectory " + path)
            return true
        }
        if !ok { throw MemoryFileSystem.notFound(path) }
    }
    func openForReading(_ path: String, bypassCache: Bool) throws(BroError) -> any ByteStream {
        try check("open", path)
        let bytes = lock.withLock { () -> [UInt8]? in
            guard let disk = files[MemoryFileSystem.key(path)] else { return nil }
            record("open \(path) \(bypassCache ? "disk" : "cache")")
            _openStreams += 1
            return !bypassCache ? cached[MemoryFileSystem.key(path)] ?? disk : disk
        }
        guard let bytes else { throw MemoryFileSystem.notFound(path) }
        return Stream(fs: self, path: path, bytes: bytes)
    }

    /// A file read a few bytes at a time ('read' fails are checked on each read).
    private final class Stream: ByteStream, @unchecked Sendable {
        private let fs: MemoryFileSystem
        private let path: String
        private let bytes: [UInt8]
        private var at = 0
        private var closed = false

        init(fs: MemoryFileSystem, path: String, bytes: [UInt8]) {
            self.fs = fs
            self.path = path
            self.bytes = bytes
        }

        func read(_ maxBytes: Int) throws(BroError) -> [UInt8] {
            try fs.check("read", path)
            fs.lock.withLock { if fs._recording { fs._reads[path, default: 0] += 1 } }
            let n = min(maxBytes, 16, bytes.count - at)
            let piece = Array(bytes[at..<at + n])
            at += n
            return piece
        }

        func close() {
            fs.lock.withLock {
                if !closed {
                    closed = true
                    fs._openStreams -= 1
                }
            }
        }
    }
    func volume(_ path: String) throws(BroError) -> VolumeInfo {
        guard let v = lock.withLock({ volumes[path] }) else { throw MemoryFileSystem.notFound(path) }
        return v
    }
}

/// Ports the pipeline's own tests never call.
struct UnusedTools: Makemkv, Matroska, DriveControl, LibraryPort, CatalogQueries, DriveLookup, MakemkvSetup, SpaceReservations {
    func reserve(_ volume: String, path: String, need: Int64, free: Int64) async throws(BroError) { fatalError("not used") }
    func scanDrives(_ cancel: CancellationToken) async throws(BroError) -> [MakemkvDrive] { fatalError("not used") }
    func listing(_ source: MakemkvSource, invocation: MakemkvInvocation, sink: any RunSink, cancel: CancellationToken) async throws(BroError) -> ListingRun { fatalError("not used") }
    func rip(_ source: MakemkvSource, title: String, destination: String, invocation: MakemkvInvocation, sink: any RunSink,
             cancel: CancellationToken) async throws(BroError) -> MakemkvRun { fatalError("not used") }
    func backup(_ source: MakemkvSource, decrypt: Bool, destination: String, invocation: MakemkvInvocation, sink: any RunSink,
                cancel: CancellationToken) async throws(BroError) -> MakemkvRun { fatalError("not used") }
    func probe(_ file: String, cancel: CancellationToken) async throws(BroError) -> MkvProbe? { fatalError("not used") }
    func remux(_ arguments: [String], cancel: CancellationToken) async throws(BroError) -> Bool { fatalError("not used") }
    func split(_ chapters: [Int], input: String, cancel: CancellationToken) async throws(BroError) -> [String]? { fatalError("not used") }
    func chapterTimes(_ file: String, cancel: CancellationToken) async throws(BroError) -> [Duration]? { fatalError("not used") }
    func eject(_ device: String) async throws(BroError) { fatalError("not used") }
    func closeTray(_ device: String) async throws(BroError) { fatalError("not used") }
    func waitForMount(_ device: String, timeout: Duration, cancel: CancellationToken) async -> String? { fatalError("not used") }
    func probeContent(_ device: String) -> DiscContent { fatalError("not used") }
    func openRaw(_ device: String) throws(BroError) -> any SectorReader { fatalError("not used") }
    func require(_ libraryId: String) throws(BroError) -> LibraryHandle { fatalError("not used") }
    func stagingDirectory(_ libraryId: String, jobId: Id) throws(BroError) -> String { fatalError("not used") }
    func commit(_ unit: UnitRecord, intent: CommitIntent, files: [UnitFile]) async throws(BroError) -> CommitResult { fatalError("not used") }
    func quarantine(_ unit: UnitRecord, intent: CommitIntent, files: [UnitFile], tag: String) async throws(BroError) -> CommitResult {
        fatalError("not used")
    }
    func archivedBefore(_ fingerprint: String) async throws(BroError) -> [UnitRecord] { fatalError("not used") }
    func find(_ driveId: String) -> DriveLocation? { fatalError("not used") }
    func invocation(_ config: Config, profile: EffectiveProfile, selectionOverride: String?, jobDirectory: String) throws(BroError) -> MakemkvInvocation {
        fatalError("not used")
    }
}

/// Gives the document a case sets (given.config, then its config ops).
@EngineActor
final class CaseConfig: ConfigSource {
    var document: JsonValue = .object([])

    func current() -> Config { Config(document: document, issues: []) }
}
