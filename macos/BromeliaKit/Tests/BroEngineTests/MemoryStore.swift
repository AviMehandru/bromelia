import BroDomain
import BroEngine
import BroFoundation
import BroPorts
import Foundation

/// Jobs and steps in memory, and the open commit intents (the rest of the store isn't needed by these tests).
/// Thread-safe.
final class MemoryStore: Store, JobRepository, StepRepository, UnitRepository, @unchecked Sendable {
    private let lock = NSLock()
    private var jobsById: [Id: JobRecord] = [:]
    private var order: [Id] = [] // jobs in the order they were inserted
    private var stepsByKey: [String: StepRecord] = [:]
    private var intents: [CommitIntent] = []
    private var unitsById: [Id: UnitRecord] = [:]

    func jobs() -> any JobRepository { self }
    func steps() -> any StepRepository { self }
    func units() -> any UnitRepository { self }
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

    /// Reads of these jobs' records and step rows fail with store.failed.
    var failJobLoads: Set<Id> = []
    var failStepLoads: Set<Id> = []

    private func readFailed() -> BroError { BroError("store.failed", [("operation", .string("read")), ("reason", .string("disk I/O error"))]) }

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
        if failJobLoads.contains(jobId) { throw readFailed() }
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
        if failStepLoads.contains(jobId) { throw readFailed() }
        return lock.withLock { stepsByKey.values.filter { $0.jobId == jobId }.sorted { $0.seq < $1.seq } }
    }

    /// An open commit intent of the job (its unit is half moved into a library).
    func openIntent(_ jobId: Id) {
        lock.withLock {
            intents.append(CommitIntent(unitId: Id("unit-" + jobId.value), jobId: jobId, staging: "/lib/.bromelia/staging/" + jobId.value,
                                        destination: "/lib/Unit", merge: false, quarantine: false, createdAt: Instant(unixMilliseconds: 0), items: []))
        }
    }

    func openIntents() async throws(BroError) -> [CommitIntent] { lock.withLock { intents } }
    func beginCommit(_ unit: UnitRecord, intent: CommitIntent) async throws(BroError) { fatalError("not used") }
    func markMoved(_ unitId: Id, seq: Int) async throws(BroError) { fatalError("not used") }
    func finishCommit(_ intent: CommitIntent, record: UnitRecord, files: [UnitFile]) async throws(BroError) { fatalError("not used") }
    /// The job's unit (ArchiveFiles.unitId), stored in that state.
    func storeUnit(_ jobId: Id, _ state: UnitState) {
        let id = ArchiveFiles.unitId(jobId)
        lock.withLock {
            unitsById[id] = UnitRecord(id: id, libraryId: "lib", jobId: jobId, path: "Movies/Unit", recordFile: "bromelia-unit.json", recordVersion: 3,
                                   state: state, status: .success, name: "Unit", kind: .movie, format: "bluray", formatCode: "BD", encrypted: false,
                                   label: "UNIT", makemkvVersion: "", bytes: 0, fileCount: 0, attempts: 1, createdAt: Instant(unixMilliseconds: 0))
        }
    }

    func get(_ unitId: Id) async throws(BroError) -> UnitRecord? { lock.withLock { unitsById[unitId] } }
    func query(_ query: UnitQuery) async throws(BroError) -> [UnitRecord] { fatalError("not used") }
    func leastRecentlyVerified(_ limit: Int) async throws(BroError) -> [UnitRecord] { fatalError("not used") }
    func markMissing(_ unitId: Id) async throws(BroError) { fatalError("not used") }

    func job(_ id: Id) -> JobRecord? { lock.withLock { jobsById[id] } }
    func stepsOf(_ id: Id) -> [StepRecord] { lock.withLock { stepsByKey.values.filter { $0.jobId == id }.sorted { $0.seq < $1.seq } } }
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
    func commit(_ unit: UnitRecord, intent: CommitIntent, files: [UnitFile], cancel: CancellationToken) async throws(BroError) -> CommitResult { fatalError("not used") }
    func quarantine(_ unit: UnitRecord, intent: CommitIntent, files: [UnitFile], tag: String, cancel: CancellationToken) async throws(BroError) -> CommitResult {
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
