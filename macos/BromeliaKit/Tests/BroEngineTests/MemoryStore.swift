import BroDomain
import BroEngine
import BroFoundation
import BroPorts
import Foundation

/// Jobs and steps in memory (the rest of the store isn't needed by these tests). Thread-safe.
final class MemoryStore: Store, JobRepository, StepRepository, @unchecked Sendable {
    private let lock = NSLock()
    private var jobsById: [Id: JobRecord] = [:]
    private var stepsByKey: [String: StepRecord] = [:]

    func jobs() -> any JobRepository { self }
    func steps() -> any StepRepository { self }
    func units() -> any UnitRepository { fatalError("not used") }
    func catalog() -> any CatalogRepository { fatalError("not used") }
    func checks() -> any CheckRepository { fatalError("not used") }
    func replicas() -> any ReplicaRepository { fatalError("not used") }
    func drives() -> any DriveRepository { fatalError("not used") }
    func lookupCache() -> any LookupCacheRepository { fatalError("not used") }
    func outbox() -> any OutboxRepository { fatalError("not used") }
    func kv() -> any KeyValueRepository { fatalError("not used") }
    func transaction<T: Sendable>(_ block: @Sendable (any Store) async throws(BroError) -> T) async throws(BroError) -> T { try await block(self) }
    func migrate() async throws(BroError) {}

    func insert(_ job: JobRecord) async throws(BroError) { lock.withLock { jobsById[job.id] = job } }
    func update(_ job: JobRecord) async throws(BroError) { lock.withLock { jobsById[job.id] = job } }
    func load(_ jobId: Id) async throws(BroError) -> JobRecord? { lock.withLock { jobsById[jobId] } }
    func active() async throws(BroError) -> [JobRecord] { lock.withLock { jobsById.values.filter { $0.state != .finished } } }
    func query(_ query: JobQuery) async throws(BroError) -> [JobRecord] { lock.withLock { Array(jobsById.values) } }

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

/// Files and folders in memory: what the engine's tests need of the FileSystem port. Thread-safe.
final class MemoryFileSystem: FileSystem, @unchecked Sendable {
    private let lock = NSLock()
    private var files: [String: [UInt8]] = [:]
    private var folders: Set<String> = ["/"]

    func exists(_ path: String) -> Bool { lock.withLock { files[path] != nil || folders.contains(path) } }
    func read(_ path: String) throws(BroError) -> [UInt8] {
        guard let b = lock.withLock({ files[path] }) else { throw BroError("fs.notFound", [("path", .string(path))]) }
        return b
    }
    func createDirectory(_ path: String, parentsMustExist: Bool) throws(BroError) {
        lock.withLock {
            var current = ""
            for part in path.split(separator: "/") {
                current += "/" + part
                folders.insert(current)
            }
        }
    }
    func writeAtomically(_ path: String, bytes: [UInt8], mode: Int) throws(BroError) {
        let folder = String(path[..<path.lastIndex(of: "/")!])
        let ok = lock.withLock { () -> Bool in
            guard folders.contains(folder) else { return false }
            files[path] = bytes
            return true
        }
        if !ok { throw BroError("fs.notFound", [("path", .string(folder))]) }
    }
    func stat(_ path: String) throws(BroError) -> FileInfo { fatalError("not used") }
    func list(_ directory: String) throws(BroError) -> [DirectoryEntry] { fatalError("not used") }
    func readRange(_ path: String, offset: Int64, length: Int) throws(BroError) -> [UInt8] { fatalError("not used") }
    func rename(_ from: String, to: String) throws(BroError) { fatalError("not used") }
    func moveMerging(_ from: String, to: String, policy: MovePolicy, onMoved: (MovedItem) -> Void) throws(BroError) -> [MovedItem] { fatalError("not used") }
    func remove(_ path: String) throws(BroError) { fatalError("not used") }
    func moveToTrash(_ path: String, trash: String) throws(BroError) { fatalError("not used") }
    func syncFile(_ path: String) throws(BroError) { fatalError("not used") }
    func syncDirectory(_ path: String) throws(BroError) { fatalError("not used") }
    func openForReading(_ path: String, bypassCache: Bool) throws(BroError) -> any ByteStream { fatalError("not used") }
    func volume(_ path: String) throws(BroError) -> VolumeInfo { fatalError("not used") }
}

/// Ports the pipeline's own tests never call.
struct UnusedTools: Makemkv, Matroska, DriveControl, LibraryPort, CatalogQueries {
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
    func archivedBefore(_ fingerprint: String) async throws(BroError) -> [UnitRecord] { fatalError("not used") }
}
