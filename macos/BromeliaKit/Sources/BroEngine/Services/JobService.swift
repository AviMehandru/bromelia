import BroDomain
import BroFoundation
import BroPorts
import Foundation

/// The jobs (plan §11.1, §13.1): it creates them, runs them through the PipelineRunner and keeps the in-memory view of the
/// ones that haven't finished; the Scheduler decides when each step runs. On the executor; the store and the log files
/// are read and written on workers. shared/fixtures/engine/job-service.cases.json.
/// - A created job is in its queue at once, in the order the jobs were created, and in the store before jobChanged
///   (queued) announces it; then it runs. `start()` runs the jobs the last engine left unfinished (after
///   RecoveryService.recover), in their queues' order.
/// - Each job's place in its queue is stored, so the order survives a restart (written in a transaction that changes only
///   the position: the runner owns the rest of the record).
/// - The continue offer comes with AutomationService, keep-apart with session F (owner, 2026-10-17/18).
@EngineActor
public final class JobService {
    /// A job this service runs, until its run ends.
    final class Active {
        var cancelRequested = false
    }

    /// Where each active job should be stored in its queue; read by the store's workers, hence the lock.
    final class Positions: @unchecked Sendable {
        private let lock = NSLock()
        private var map: [Id: Int] = [:]

        subscript(id: Id) -> Int? {
            get { lock.withLock { map[id] } }
            set { lock.withLock { map[id] = newValue } }
        }
    }

    private let executor: EngineExecutor
    private let bus: EventBus
    private let store: any Store
    private let scheduler: Scheduler
    private let runner: PipelineRunner
    private let clock: EngineClock
    private let ids: any IdGenerator
    private let fs: any FileSystem
    private let data: String
    private var active: [Id: Active] = [:]
    private let positions = Positions()
    /// The positions last asked of the store.
    private var asked: [Id: Int] = [:]
    private var queues: Subscription?

    /// `dataDirectory`: the engine's data folder; a job's log is <data>/jobs/<id>/log.jsonl.
    public init(executor: EngineExecutor, bus: EventBus, store: any Store, scheduler: Scheduler, runner: PipelineRunner, clock: EngineClock,
                ids: any IdGenerator, fs: any FileSystem, dataDirectory: String) {
        self.executor = executor
        self.bus = bus
        self.store = store
        self.scheduler = scheduler
        self.runner = runner
        self.clock = clock
        self.ids = ids
        self.fs = fs
        self.data = dataDirectory
    }

    // MARK: Commands

    /// Runs the jobs the store has unfinished (what RecoveryService left queued), in their queues' order. Call it once,
    /// after recover().
    public func start() async throws(BroError) {
        if queues == nil { queues = bus.subscribe(types: [.queueChanged]) { [unowned self] _ in onQueueChanged() } }
        let store = store
        for job in try await executor.worker({ () async throws(BroError) -> [JobRecord] in try await store.jobs().active() }) {
            if active[job.id] != nil { continue }
            admit(job.id, queue: job.queue)
            asked[job.id] = job.position // what the store has: onQueueChanged writes the place it has now
            launch(job)
        }
        onQueueChanged()
    }

    /// A new job for the request: queued, stored, announced (jobChanged), then run.
    public func create(_ request: JobRequest) async throws(BroError) -> JobDto {
        let id = ids.next()
        let queue = JobPlanner.queue(request.kind)
        let steps = JobPlanner.initial(request)
        let position = admit(id, queue: queue)
        let record = JobRecord(id: id, kind: request.kind, parentId: request.parentId, state: .queued, queue: queue, position: position,
                               driveId: request.driveId, mediaGeneration: request.mediaGeneration, automatic: request.automatic,
                               title: request.title, request: request, plan: steps.isEmpty ? nil : JobPlan(steps: steps), unitId: request.unitId,
                               createdAt: clock.now())
        let store = store
        do throws(BroError) {
            try await executor.worker { () async throws(BroError) in try await store.jobs().insert(record) }
        } catch {
            forget(id)
            scheduler.finish(id)
            throw error
        }
        if asked[id] != position { savePosition(id) } // the queue moved while the record was written
        var dto = JobDto.of(record)
        dto.position = scheduler.position(id)
        bus.publish(.jobChanged, data: .object([("job", JobDto.json(dto))]))
        launch(record)
        return dto
    }

    /// Cancels the job: a waiting or running one ends cancelled (job.cancelledByUser); one nothing runs is finished as
    /// cancelled at once; a finished one stays as it is. job.notFound when there is none.
    public func cancel(_ jobId: Id) async throws(BroError) {
        if let a = active[jobId] {
            a.cancelRequested = true
            runner.cancel(jobId)
            return
        }
        let record = try await load(jobId)
        if record.state == .finished || active[jobId] != nil { return }
        try await finishCancelled(record)
    }

    /// Runs a finished job again (plan §13.1's retry(id, fromStep?); owner 2026-10-17/18).
    /// - Without a step: one that failed, was cancelled or was skipped, as a new job with the same request, started by hand
    ///   (not automatic, no media generation); an interrupted one is continued: the same job from its first step.
    /// - From a step: one that failed, was cancelled or was interrupted, the same job, from that step (job.unknownStep {step}
    ///   when its plan has none; server.badRequest when a step before it isn't done). A step up to Release starts it from its
    ///   first step instead: the disc is read again (Acquire then keeps what an earlier attempt finished of the same disc).
    ///
    /// The same job's steps from there are stored pending (their checkpoints kept), its plan cut back after Plan when Plan
    /// runs again, and its record queued again (no outcome, by hand, no media generation; acquisition up to Release, else
    /// processing) with job.retried {step} in its log, then it runs like a new one. job.notFound; job.notRetryable for any
    /// other job (read errors and failed post steps get retries of their own). While the job has an open commit intent
    /// (its files are partly in the library), only a retry from Commit is taken, which finishes it: any other is
    /// job.commitOpen (a step before Commit would change staging under the intent; a new job would archive the disc again
    /// beside it).
    public func retry(_ jobId: Id, fromStep: StepKind? = nil) async throws(BroError) -> JobDto {
        let record = try await load(jobId)
        let notRetryable = BroError(MessageCode.wire(.jobNotRetryable), [("jobId", .string(jobId.value))])
        guard record.state == .finished, active[jobId] == nil, let outcome = record.outcome else { throw notRetryable }
        if fromStep == nil && [.failed, .cancelled, .skipped].contains(outcome) {
            var request = record.request
            request.automatic = false
            request.mediaGeneration = nil
            try await notCommitting(jobId)
            return try await create(request)
        }
        guard [.failed, .cancelled, .interrupted].contains(outcome) else { throw notRetryable }

        let steps = record.plan?.steps ?? JobPlanner.initial(record.request)
        var seq = 0
        if let fromStep {
            guard let at = steps.firstIndex(of: fromStep) else {
                throw BroError(MessageCode.wire(.jobUnknownStep), [("step", .string(fromStep.rawValue))])
            }
            seq = at
        }
        let store = store
        let rows = try await executor.worker { () async throws(BroError) -> [StepRecord] in try await store.steps().load(jobId) }
        if seq > JobPlanner.done(rows).count {
            throw BroError(MessageCode.wire(.serverBadRequest), [("detail", .string("fromStep \(fromStep!.rawValue): a step before it isn't done"))])
        }
        let release = steps.firstIndex(of: .release)
        if release == nil || seq <= release! { seq = 0 }
        if let commit = steps.firstIndex(of: .commit), seq < commit { try await notCommitting(jobId) }
        for row in rows where row.seq >= seq {
            var pending = row
            pending.state = .pending
            pending.startedAt = nil
            pending.finishedAt = nil
            pending.output = nil
            pending.error = nil
            let save = pending
            try await executor.worker { () async throws(BroError) in try await store.steps().save(save) }
        }
        var again = record
        if var plan = again.plan, let planned = steps.firstIndex(of: .plan), seq <= planned {
            plan.steps = Array(steps[...planned])
            again.plan = plan
        }
        let queue: Queue = release.map { seq > $0 } == true ? .processing : JobPlanner.queue(record.kind)
        let position = admit(jobId, queue: queue)
        again.state = .queued
        again.outcome = nil
        again.error = nil
        again.finishedAt = nil
        again.decision = nil
        again.blockedBy = nil
        again.queue = queue
        again.position = position
        again.automatic = false
        again.mediaGeneration = nil
        again.request.automatic = false
        again.request.mediaGeneration = nil
        let queued = again
        let line = LogLineDto(seq: 0, t: clock.now(), severity: .info, code: .jobRetried, params: [("step", .string(steps[seq].rawValue))])
        let fs = fs, directory = data + "/jobs/" + jobId.value
        do throws(BroError) {
            try await executor.worker { () async throws(BroError) in try await store.jobs().update(queued) }
            try await executor.worker { () throws(BroError) in try JobLog.append(fs, jobDirectory: directory, lines: [line]) }
        } catch {
            forget(jobId)
            scheduler.finish(jobId)
            throw error
        }
        if asked[jobId] != position { savePosition(jobId) }
        var dto = JobDto.of(queued)
        dto.position = scheduler.position(jobId)
        bus.publish(.jobChanged, data: .object([("job", JobDto.json(dto))]))
        launch(queued)
        return dto
    }

    /// Answers the job's question `decisionId`. job.notFound; job.notAwaitingDecision when the job isn't waiting for that
    /// question; server.badRequest for an answer it doesn't offer.
    public func decide(_ jobId: Id, decisionId: String, answer: String) async throws(BroError) {
        if active[jobId] == nil { _ = try await load(jobId) }
        guard runner.job(jobId)?.decision?.id == decisionId else {
            throw BroError(MessageCode.wire(.jobNotAwaitingDecision), [("jobId", .string(jobId.value))])
        }
        try runner.decide(jobId, answer: answer)
    }

    /// Moves a job that waits to start a step `by` places in its queue (negative: earlier), stopping at the ends.
    /// job.notFound; job.notQueued for a job that runs a step, waits for an answer or has finished.
    public func move(_ jobId: Id, by: Int) async throws(BroError) {
        try await waiting(jobId)
        try scheduler.move(jobId, by: by)
    }

    /// Puts a job that waits to start a step first in its queue; the acquisitions cap no longer holds it back.
    /// job.notFound; job.notQueued as for `move`.
    public func startNow(_ jobId: Id) async throws(BroError) {
        try await waiting(jobId)
        try scheduler.startNow(jobId)
    }

    /// A page of jobs, newest first: running jobs as they run (progress, blockedBy, decision), waiting ones with their
    /// place in the queue. The states are the stored ones (queued, running, finished). server.badRequest for a cursor that
    /// isn't one or a limit outside 1–500.
    public func query(_ query: JobQueryDto) async throws(BroError) -> JobPageDto {
        guard (1...JobQueryDto.maxLimit).contains(query.limit) else { throw JobService.badRequest("limit \(query.limit)") }
        var offset = 0
        if let cursor = query.cursor {
            guard !cursor.isEmpty, cursor.allSatisfy({ $0.isASCII && $0.isNumber }), let n = Int(cursor) else {
                throw JobService.badRequest("cursor " + cursor)
            }
            offset = n
        }
        let q = JobQuery(states: query.states, outcomes: query.outcomes, kind: query.kind, queue: query.queue, driveId: query.driveId,
                         unitId: query.unitId, limit: query.limit, offset: offset)
        let store = store
        let (records, total) = try await executor.worker { () async throws(BroError) -> ([JobRecord], Int) in
            (try await store.jobs().query(q), try await store.jobs().count(q))
        }
        let items = records.map { r -> JobDto in
            var dto = runner.job(r.id) ?? JobDto.of(r)
            if [.queued, .waitingForResources, .blocked].contains(dto.state) { dto.position = scheduler.position(r.id) }
            return dto
        }
        let next = !records.isEmpty && offset + records.count < total ? String(offset + records.count) : nil
        return JobPageDto(items: items, next: next, total: total)
    }

    /// The job's log lines after seq `after` (0: all), including a running step's lines not yet written. job.notFound.
    public func log(_ jobId: Id, after: Int64) async throws(BroError) -> [LogLineDto] {
        var lines = runner.log(jobId)
        if lines == nil {
            if active[jobId] == nil { _ = try await load(jobId) }
            let path = data + "/jobs/" + jobId.value + "/" + JobLog.fileName
            let fs = fs
            lines = try await executor.worker { () throws(BroError) -> [LogLineDto] in fs.exists(path) ? JobLog.decode(try fs.read(path)) : [] }
        }
        return lines!.filter { $0.seq > after }
    }

    // MARK: Running

    /// Puts the job in its queue (in call order: before any await) and in the active view; its place.
    @discardableResult
    private func admit(_ jobId: Id, queue: Queue) -> Int {
        active[jobId] = Active()
        scheduler.submit(jobId, queue: queue)
        let position = scheduler.position(jobId) ?? 0
        asked[jobId] = position
        positions[jobId] = position
        return position
    }

    /// Starts the job's run, or finishes it as cancelled when a cancel came first. The run starts as a task of its own
    /// (later, on the actor): a cancel until then is seen there; from its start the runner has the job live.
    private func launch(_ record: JobRecord) {
        guard let a = active[record.id] else { return }
        executor.spawn { [self] in
            if a.cancelRequested {
                try? await finishCancelledAndForget(record)
                return
            }
            do throws(BroError) {
                _ = try await runner.run(record.id)
            } catch {
                scheduler.finish(record.id) // its record was gone: nothing ran
            }
            forget(record.id)
        }
    }

    private func finishCancelledAndForget(_ record: JobRecord) async throws(BroError) {
        defer {
            scheduler.finish(record.id)
            forget(record.id)
        }
        try await finishCancelled(record)
    }

    /// Finishes a job nothing runs as cancelled (job.cancelledByUser).
    private func finishCancelled(_ record: JobRecord) async throws(BroError) {
        let decided = JobOutcome.decide([], cancelled: true, policy: .default)
        var finished = record
        finished.state = .finished
        finished.outcome = decided.outcome
        finished.error = decided.error
        finished.decision = nil
        finished.blockedBy = nil
        finished.finishedAt = clock.now()
        let store = store
        let done = finished
        try await executor.worker { () async throws(BroError) in try await store.jobs().update(done) }
        bus.publish(.jobFinished, data: .object([("job", JobDto.json(JobDto.of(finished)))]))
    }

    private func forget(_ jobId: Id) {
        active.removeValue(forKey: jobId)
        asked.removeValue(forKey: jobId)
        positions[jobId] = nil
    }

    /// Stores the place of every active job whose place in its queue changed.
    private func onQueueChanged() {
        let orders = scheduler.queues()
        for order in [orders.acquisition, orders.processing, orders.maintenance] {
            for (i, id) in order.enumerated() {
                guard let was = asked[id], was != i else { continue } // not admitted yet, or in place
                asked[id] = i
                positions[id] = i
                savePosition(id)
            }
        }
    }

    /// Writes the job's place as it is when the write runs (so a later move wins over an earlier write that lands last),
    /// in a transaction that changes nothing else. Best effort: the order only matters after a restart.
    private func savePosition(_ jobId: Id) {
        let store = store
        let positions = positions
        executor.spawn { [executor] in
            _ = try? await executor.worker { () async throws(BroError) -> Bool in
                try await store.transaction { (s: any Store) async throws(BroError) -> Bool in
                    guard let position = positions[jobId], var record = try await s.jobs().load(jobId), record.state != .finished,
                          record.position != position else { return false }
                    record.position = position
                    try await s.jobs().update(record)
                    return true
                }
            }
        }
    }

    // MARK: Helpers

    /// The stored job; job.notFound when there is none.
    private func load(_ jobId: Id) async throws(BroError) -> JobRecord {
        let store = store
        guard let record = try await executor.worker({ () async throws(BroError) -> JobRecord? in try await store.jobs().load(jobId) }) else {
            throw BroError(MessageCode.wire(.jobNotFound), [("jobId", .string(jobId.value))])
        }
        return record
    }

    /// job.commitOpen while the job has an open commit intent.
    private func notCommitting(_ jobId: Id) async throws(BroError) {
        let store = store
        let intents = try await executor.worker { () async throws(BroError) -> [CommitIntent] in try await store.units().openIntents() }
        if intents.contains(where: { $0.jobId == jobId }) {
            throw BroError(MessageCode.wire(.jobCommitOpen), [("jobId", .string(jobId.value))])
        }
    }

    /// Fails unless the job waits to start a step (queued, waiting for resources, blocked).
    private func waiting(_ jobId: Id) async throws(BroError) {
        guard active[jobId] != nil else {
            _ = try await load(jobId)
            throw JobService.notQueued(jobId)
        }
        let state = runner.job(jobId)?.state ?? .queued
        guard [.queued, .waitingForResources, .blocked].contains(state), scheduler.position(jobId) != nil else { throw JobService.notQueued(jobId) }
    }

    private static func notQueued(_ jobId: Id) -> BroError { BroError(MessageCode.wire(.jobNotQueued), [("jobId", .string(jobId.value))]) }

    private static func badRequest(_ detail: String) -> BroError { BroError(MessageCode.wire(.serverBadRequest), [("detail", .string(detail))]) }
}
