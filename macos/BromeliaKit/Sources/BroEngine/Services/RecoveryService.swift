import BroDomain
import BroFoundation
import BroPorts
import Foundation

/// Runs once at startup, before the libraries start (plan §11.1, §15.1, §20.4, §15.8; review L3-3, L3-9). It sorts out
/// the jobs the last engine left unfinished by where each stopped and what its step can do after a stop (its
/// Resumability), rolls open commit intents forward, sets aside staging folders no job will continue, and removes the
/// temporary files a stopped write leaves. Library work runs per library and may take at most 30 s of the startup: a
/// share that doesn't answer can't keep the engine from starting, and its work goes on.
/// shared/fixtures/engine/recovery-service.cases.json.
@EngineActor
public final class RecoveryService {
    private static let patience: Int64 = 30_000

    private let executor: EngineExecutor
    private let bus: EventBus
    private let problems: ProblemService
    private let store: any Store
    private let registry: StepRegistry
    private let library: LibraryService
    private let config: any ConfigSource
    private let clock: EngineClock
    private let fs: any FileSystem
    private let isolation: any SettingsIsolation
    private let renderer: any MessageRenderer
    private let data: String
    private let logLock = NSLock()

    /// `dataDirectory`: the engine's data folder; a job's folder is <data>/jobs/<id>.
    public init(executor: EngineExecutor, bus: EventBus, problems: ProblemService, store: any Store, registry: StepRegistry,
                library: LibraryService, config: any ConfigSource, clock: EngineClock, fs: any FileSystem, isolation: any SettingsIsolation,
                renderer: any MessageRenderer, dataDirectory: String) {
        self.executor = executor
        self.bus = bus
        self.problems = problems
        self.store = store
        self.registry = registry
        self.library = library
        self.config = config
        self.clock = clock
        self.fs = fs
        self.isolation = isolation
        self.renderer = renderer
        self.data = dataDirectory
    }

    /// recovery.summary{count} when the last engine left jobs unfinished; nil otherwise.
    public func recover() async throws(BroError) -> BroMessage? {
        let store = store
        let jobs = try await executor.worker { () async throws(BroError) -> [JobRecord] in try await store.jobs().active() }
        let homes = jobs.map { jobDirectory($0.id) + "/home" }
        let isolation = isolation
        let found = try await executor.worker { () throws(BroError) -> [BroMessage] in isolation.recover(homes) }
        for problem in found {
            let path = problem.params.first { $0.key == "path" }?.value.string ?? ""
            if let owner = jobs.first(where: { path.hasPrefix(jobDirectory($0.id) + "/") }) {
                try await log(owner.id, problem)
            } else {
                _ = problems.raise(problem)
            }
        }

        var unfinished = Set<String>()
        for job in jobs where try await recoverJob(job) { unfinished.insert(job.id.value) }

        let libraries = Config.libraries(config.current())
        let started = clock.now().unixMilliseconds
        let unfinishedNow = unfinished
        await executor.suspend { (r: Resumption<Void>) in
            var pending = libraries.count + 1
            var resumed = false
            var timer: EngineTimer?
            let finish: @EngineActor () -> Void = {
                guard !resumed else { return }
                resumed = true
                timer?.cancel()
                r.resume(returning: ())
            }
            let oneDone: @EngineActor () -> Void = {
                pending -= 1
                if pending == 0 { finish() }
            }
            timer = clock.timer(.at(instant: Instant(unixMilliseconds: started + Self.patience))) { finish() }
            for lib in libraries {
                executor.spawn { [self] in
                    await tidy(lib.id, lib.path, unfinishedNow)
                    oneDone()
                }
            }
            executor.spawn { [self] in
                await removeLeftovers(data, recursive: true)
                oneDone()
            }
        }
        return jobs.isEmpty ? nil : BroMessage(.recoverySummary, [("count", .integer(Int64(jobs.count)))], severity: .warning)
    }

    // ---- jobs ------------------------------------------------------------------------------------------------------------

    /// Requeues, resumes or finishes the job; whether it is still unfinished.
    private func recoverJob(_ job: JobRecord) async throws(BroError) -> Bool {
        let store = store
        let jobId = job.id
        let rows = try await executor.worker { () async throws(BroError) -> [StepRecord] in try await store.steps().load(jobId) }
        let done = JobPlanner.done(rows)
        var results = done.map { StepResult(kind: $0.kind, state: $0.state, error: $0.error, readErrors: $0.output?.readErrors ?? 0) }
        let steps = job.plan?.steps ?? JobPlanner.initial(job.request)
        let row = rows.first { $0.seq == done.count }

        // The runner stopped between a step that ended (or the last one) and the job's record.
        if done.count >= steps.count || row?.state == .failed || row?.state == .cancelled {
            if let row, done.count < steps.count { results.append(StepResult(kind: row.kind, state: row.state, error: row.error)) }
            try await finish(job, JobOutcome.decide(results, cancelled: row?.state == .cancelled, policy: .default), nil)
            return false
        }

        let kind = steps[done.count]
        let started = row?.state == .running || row?.state == .interrupted
        let registered = registry.step(kind)
        let resumability = registered?.resumability ?? .notResumable
        let step: [(key: String, value: JsonValue)] = [("step", .string(kind.rawValue))]
        let resumes = resumability == .resumeFrom && registered?.resumeCheckpoint != nil
            && row?.checkpoint?["kind"]?.string == registered?.resumeCheckpoint
        if let row, started, resumability == .notResumable || (resumability == .resumeFrom && !resumes) {
            var interrupted = row
            interrupted.state = .interrupted
            try await save(interrupted)
            results.append(StepResult(kind: kind, state: .interrupted))
            try await finish(job, JobOutcome.decide(results, cancelled: false, policy: .default), BroMessage(.recoveryInterrupted, step, severity: .error))
            return false
        }
        if started && resumability == .resumeFrom {
            try await requeue(job, job.queue, BroMessage(.recoveryResumed, step))
            return true
        }
        // Past Acquire, or keeping an interrupted job's files apart (which needs no drive): resumed.
        if done.contains(where: { $0.kind == .acquire }) || kind == .keepApart {
            let queue: Queue = done.contains(where: { $0.kind == .release }) || kind == .keepApart ? .processing : job.queue
            try await requeue(job, queue, BroMessage(.recoveryResumed, step))
            return true
        }

        // Before Acquire. A media generation doesn't outlive the engine (owner, 2026-10-14): an automatic job can't know
        // its disc is still there (the new run's insert starts a fresh job), and a manual one reads the drive again.
        var job = job
        if job.request.driveId != nil && job.request.automatic {
            if var row, started {
                row.state = .interrupted
                try await save(row)
            }
            let error = BroError(MessageCode.wire(.discChanged), [("why", BroMessage(.discChangedGeneration).toJson())])
            try await finish(job, Decided(outcome: .cancelled, error: error), BroMessage(.discChanged, error.params, severity: .error))
            return false
        }
        if job.request.driveId != nil {
            for var r in rows {
                r.state = .pending
                r.startedAt = nil
                r.finishedAt = nil
                r.output = nil
                r.error = nil
                r.checkpoint = nil
                try await save(r)
            }
            job.plan = nil
            job.mediaGeneration = nil
            job.request.mediaGeneration = nil
        }
        try await requeue(job, job.queue, BroMessage(.recoveryRequeued))
        return true
    }

    private func requeue(_ job: JobRecord, _ queue: Queue, _ line: BroMessage) async throws(BroError) {
        var record = job
        record.state = .queued
        record.queue = queue
        record.decision = nil
        record.blockedBy = nil
        try await update(record)
        try await log(job.id, line)
        bus.publish(.jobChanged, data: .object([("job", JobDto.json(JobDto.of(record)))]))
    }

    private func finish(_ job: JobRecord, _ decided: Decided, _ line: BroMessage?) async throws(BroError) {
        var record = job
        record.state = .finished
        record.outcome = decided.outcome
        record.error = decided.error
        record.decision = nil
        record.blockedBy = nil
        record.finishedAt = clock.now()
        try await update(record)
        if let line { try await log(job.id, line) }
        bus.publish(.jobFinished, data: .object([("job", JobDto.json(JobDto.of(record)))]))
    }

    private func update(_ record: JobRecord) async throws(BroError) {
        let store = store
        try await executor.worker { () async throws(BroError) in try await store.jobs().update(record) }
    }

    private func save(_ step: StepRecord) async throws(BroError) {
        let store = store
        try await executor.worker { () async throws(BroError) in try await store.steps().save(step) }
    }

    /// Appends the line to the job's log.jsonl, on a worker (one append at a time: libraries are tidied side by side).
    private func log(_ jobId: Id, _ message: BroMessage) async throws(BroError) {
        let line = LogLineDto(seq: 0, t: clock.now(), severity: message.severity, code: message.code, params: message.params)
        let (fs, lock, directory) = (fs, logLock, jobDirectory(jobId))
        try await executor.worker { () throws(BroError) in
            let failure: BroError? = lock.withLock {
                do throws(BroError) {
                    try JobLog.append(fs, jobDirectory: directory, lines: [line])
                    return nil
                } catch {
                    return error
                }
            }
            if let failure { throw failure }
        }
    }

    // ---- libraries and files ---------------------------------------------------------------------------------------------

    /// One library: its open intents rolled forward, then its staging folders tidied and the ones no job will continue
    /// set aside. An offline library is left alone (I2). On the executor; the file and store work on workers.
    private func tidy(_ libraryId: String, _ root: String, _ unfinished: Set<String>) async {
        let store = store
        let intents = (try? await executor.worker { () async throws(BroError) -> [CommitIntent] in try await store.units().openIntents() }) ?? []
        for intent in intents {
            let unitId = intent.unitId
            let unit = try? await executor.worker { () async throws(BroError) -> UnitRecord? in try await store.units().get(unitId) }
            guard unit?.libraryId == libraryId else { continue }
            do throws(BroError) {
                if try await library.rollForward(intent) == nil { continue } // the job's Commit finished it meanwhile
                try? await log(intent.jobId, BroMessage(.commitRolledForward, [("unit", .string(intent.unitId.value))], severity: .warning))
                await removeLeftovers(intent.destination, recursive: false)
            } catch {
                // The intent stays: the job's Commit step rolls it forward when it runs again.
                try? await log(intent.jobId, BroMessage(MessageCode(rawValue: error.code) ?? .internalUnexpected, error.params, severity: .warning))
            }
        }
        let open = (try? await executor.worker { () async throws(BroError) -> [CommitIntent] in try await store.units().openIntents() }) ?? []
        let committing = Set(open.map(\.staging))
        guard let folders = try? await library.stagingFolders(libraryId) else { return }
        for folder in folders {
            let path = root + "/.bromelia/staging/" + folder
            await removeLeftovers(path, recursive: true)
            if unfinished.contains(folder) || committing.contains(path) { continue }
            let job = try? await executor.worker { () async throws(BroError) -> JobRecord? in try await store.jobs().load(Id(folder)) }
            if job?.outcome == .interrupted { continue } // kept so it can be continued (owner, 2026-10-03)
            let lines = Notes.render(Id(folder), outcome: job?.outcome ?? .interrupted, error: job?.error, readErrors: [], logs: [])
            let note = Array(lines.map { renderer.render($0) + "\n" }.joined().utf8)
            let title = Sanitizer.component(job?.title ?? "")
            _ = try? await library.setAside(libraryId, folder: folder, name: title.isEmpty ? folder : title, note: note)
            // a failure leaves it hidden in staging; the next start tries again
        }
    }

    /// Removes what a stopped write leaves (ArchiveFiles.isLeftover) from the folder, and its subfolders when `recursive`.
    private func removeLeftovers(_ folder: String, recursive: Bool) async {
        let fs = fs
        _ = try? await executor.worker { () throws(BroError) in Self.removeLeftoversNow(fs, folder, recursive) }
    }

    private nonisolated static func removeLeftoversNow(_ fs: any FileSystem, _ folder: String, _ recursive: Bool) {
        guard fs.exists(folder), let entries = try? fs.list(folder) else { return }
        for e in entries.sorted(by: { Array($0.name.utf8).lexicographicallyPrecedes(Array($1.name.utf8)) }) {
            let path = folder + "/" + e.name
            if e.isDirectory {
                if recursive { removeLeftoversNow(fs, path, true) }
            } else if ArchiveFiles.isLeftover(e.name) {
                try? fs.remove(path)
            }
        }
    }

    private func jobDirectory(_ jobId: Id) -> String { data + "/jobs/" + jobId.value }
}
