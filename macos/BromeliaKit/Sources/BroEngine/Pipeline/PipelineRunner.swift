import BroDomain
import BroFoundation
import BroPorts

/// The only code that moves a job between states (plan §12.3), on the executor. Today's JobRunner.run()/execute()
/// becomes JobPlanner (which steps), the step types (what each does), JobOutcome.decide (the result) and this loop
/// (the order, the resources, and the durable record of every step). shared/fixtures/engine/pipeline-runner.cases.json.
@EngineActor
public final class PipelineRunner {
    /// A job while it runs.
    @EngineActor
    final class Live {
        /// Loaded when the run starts; until then the job is already live (it can be cancelled).
        var loaded: JobRecord?
        var record: JobRecord {
            get { loaded! }
            set { loaded = newValue }
        }
        let cancel = CancellationSource()
        var steps: [StepKind] = []
        var seq = -1
        var progress: ProgressDto?
        var pendingProgress: ProgressDto?
        var blockedBy: BroMessage?
        var decision: (question: DecisionDto, answer: CheckedContinuation<Result<String, BroError>, Never>)?
        var logSeq: Int64 = 0
        /// The log file as an earlier run (or recovery) left it; this run's lines follow.
        var logBefore: [UInt8] = []
        var log: [LogLineDto] = []
        var directoryMade = false
        var logRead = false
        var config: Config?
        /// The outputs of the steps done so far, for the Release step at the end.
        var outputs: [StepOutput] = []
    }

    private let executor: EngineExecutor
    private let bus: EventBus
    private let scheduler: Scheduler
    private let store: any Store
    private let registry: StepRegistry
    private let environment: StepEnvironment
    private let clock: EngineClock
    private var coalescer: Coalescer?
    private var live: [Id: Live] = [:]

    public init(executor: EngineExecutor, bus: EventBus, scheduler: Scheduler, store: any Store, registry: StepRegistry,
                environment: StepEnvironment, clock: EngineClock) {
        self.executor = executor
        self.bus = bus
        self.scheduler = scheduler
        self.store = store
        self.registry = registry
        self.environment = environment
        self.clock = clock
        coalescer = Coalescer(clock: clock, interval: Duration(seconds: 0.1)) { [unowned self] key, _ in self.emitProgress(Id(key)) }
    }

    // MARK: Commands

    /// Runs the job to its end, or until it is cancelled; returns how it ended. The job is live (cancel reaches it) as soon
    /// as this is called, before its record is loaded. job.notFound when there is no such job.
    public func run(_ jobId: Id) async throws(BroError) -> Decided {
        let l = Live()
        live[jobId] = l
        let store = store
        do throws(BroError) {
            guard let record = try await executor.worker({ () async throws(BroError) -> JobRecord? in try await store.jobs().load(jobId) }) else {
                throw BroError(MessageCode.wire(.jobNotFound), [("jobId", .string(jobId.value))])
            }
            l.loaded = record
        } catch {
            live.removeValue(forKey: jobId)
            l.cancel.close()
            throw error
        }
        var decided: Decided
        do throws(BroError) {
            decided = try await runSteps(l)
        } catch {
            decided = Decided(outcome: .failed, error: error)
        }
        await releaseAtEnd(l, decided)
        await finish(l, decided)
        live.removeValue(forKey: jobId)
        l.cancel.close()
        return decided
    }

    /// Cancels a running job: a waiting request or a decision ends at once, a running step sees its token.
    public func cancel(_ jobId: Id) {
        guard let l = live[jobId] else { return }
        l.cancel.cancel()
        if let d = l.decision {
            l.decision = nil
            d.answer.resume(returning: .failure(BroError(MessageCode.wire(.jobCancelled))))
        }
    }

    /// The job as it runs (progress, blockedBy, decision); nil when it isn't running here or its record isn't loaded yet.
    public func job(_ jobId: Id) -> JobDto? {
        guard let l = live[jobId], l.loaded != nil else { return nil }
        return dto(l)
    }

    /// The running job's log: what the file held when it started, then this run's lines, written or not; nil when it
    /// isn't running here or hasn't read its log yet.
    public func log(_ jobId: Id) -> [LogLineDto]? {
        guard let l = live[jobId], l.logRead else { return nil }
        return JobLog.decode(l.logBefore) + l.log
    }

    /// Answers the job's question. job.notAwaitingDecision when it has none; server.badRequest for an answer the
    /// question doesn't offer.
    public func decide(_ jobId: Id, answer: String) throws(BroError) {
        guard let l = live[jobId], let d = l.decision else {
            throw BroError(MessageCode.wire(.jobNotAwaitingDecision), [("jobId", .string(jobId.value))])
        }
        guard d.question.answers.contains(where: { $0.id == answer }) else {
            throw BroError(MessageCode.wire(.serverBadRequest), [("detail", .string("answer " + answer))])
        }
        l.decision = nil
        setState(l, .running)
        d.answer.resume(returning: .success(answer))
    }

    // MARK: The loop

    private func runSteps(_ l: Live) async throws(BroError) -> Decided {
        let jobId = l.record.id
        let store = store
        var plan: JobPlan
        if let p = l.record.plan {
            plan = p
        } else {
            let initial = JobPlanner.initial(l.record.request)
            if initial.isEmpty { throw PipelineRunner.unexpected("no steps for a \(l.record.kind.rawValue) job yet") }
            plan = JobPlan(steps: initial)
            var r = l.record
            r.plan = plan
            try await save(l, r)
        }
        l.steps = plan.steps
        l.config = environment.config.current()
        let fs = environment.fs
        let logFile = jobDirectory(jobId) + "/" + JobLog.fileName
        l.logBefore = try await executor.worker { () throws(BroError) -> [UInt8] in fs.exists(logFile) ? try fs.read(logFile) : [] }
        l.logSeq = Int64(l.logBefore.filter { $0 == UInt8(ascii: "\n") }.count)
        l.logRead = true
        l.directoryMade = !l.logBefore.isEmpty
        let done = JobPlanner.done(try await executor.worker { () async throws(BroError) -> [StepRecord] in try await store.steps().load(jobId) })
        l.outputs = done.map { $0.output ?? StepOutput(kind: $0.kind, data: .object([])) }
        var results = done.map { StepResult(kind: $0.kind, state: $0.state, error: $0.error) }
        scheduler.submit(jobId, queue: l.record.queue)
        var cancelled = false
        var seq = done.count
        while seq < l.steps.count {
            if l.cancel.token.isCancelled { // cancelled between steps, or before the first
                cancelled = true
                break
            }
            let kind = l.steps[seq]
            guard let step = registry.step(kind) else { throw PipelineRunner.unexpected("no step \(kind.rawValue)") }
            l.seq = seq
            coalescer?.drop(jobId.value) // a new step's first progress goes out at once
            l.progress = ProgressDto(step: kind, stepIndex: seq, stepCount: l.steps.count, fraction: Double(seq) / Double(l.steps.count),
                                     currentFraction: nil, operation: BroMessage(.jobOpStarting))
            var snapshotPlan = plan
            snapshotPlan.steps = l.steps
            let snapshot = JobSnapshot(id: jobId, request: l.record.request, plan: snapshotPlan, outputs: l.outputs)
            let lease: Lease
            do throws(BroError) {
                lease = try await scheduler.acquire(jobId, resources: step.resources(snapshot), onWait: { [unowned self] wait in self.onWait(l, wait) },
                                                    cancel: l.cancel.token)
            } catch {
                if error.code == MessageCode.wire(.jobCancelled) {
                    cancelled = true
                    break
                }
                throw error
            }
            l.blockedBy = nil
            let started = clock.now()
            var running = l.record
            running.state = .running
            running.startedAt = running.startedAt ?? started
            try await save(l, running)
            setState(l, .running)
            let startRecord = StepRecord(jobId: jobId, seq: seq, kind: kind, state: .running, attempt: 1, startedAt: started)
            try await executor.worker { () async throws(BroError) in try await store.steps().save(startRecord) }

            var report: StepReport?
            var thrown: BroError?
            let context = makeContext(l, snapshot, seq)
            do throws(BroError) {
                report = try await executor.worker { () async throws(BroError) -> StepReport in try await step.run(context) }
            } catch {
                thrown = error
            }
            scheduler.release(lease)
            coalescer?.flush(jobId.value)

            let error = thrown ?? report?.error
            let state: StepState = error != nil ? (l.cancel.token.isCancelled ? .cancelled : .failed) : report!.skipped ? .skipped : .succeeded
            let output = StepOutput(kind: kind, data: report?.data ?? .object([]), summary: report?.summary)
            let endRecord = StepRecord(jobId: jobId, seq: seq, kind: kind, state: state, attempt: 1, startedAt: started, finishedAt: clock.now(),
                                       output: output, error: error)
            try await executor.worker { () async throws(BroError) in try await store.steps().save(endRecord) }
            bus.publish(.stepFinished, data: .object([("job", .string(jobId.value)), ("seq", .integer(Int64(seq))), ("kind", .string(kind.rawValue)),
                                                      ("state", .string(state.rawValue))]))
            try await writeLog(l)
            l.outputs.append(output)
            results.append(StepResult(kind: kind, state: state, error: error, readErrors: report?.readErrors ?? 0, quarantined: report?.quarantined ?? false,
                                      notice: report?.notice, skip: report?.skipJob, affectsOutcome: report?.affectsOutcome ?? true))
            if kind == .release && state == .succeeded {
                scheduler.releaseDrives(jobId)
                scheduler.moveTo(jobId, queue: .processing)
                l.record.queue = .processing
            }
            if state == .cancelled {
                cancelled = true
                break
            }
            if state == .failed && !JobOutcome.afterCommit(kind) { break }
            if let report, report.plan != nil || !report.appendSteps.isEmpty {
                l.steps += report.appendSteps
                plan = report.plan ?? plan
                plan.steps = l.steps
                var r = l.record
                r.plan = plan
                r.mode = plan.mode ?? r.mode
                try await save(l, r)
            }
            if report?.skipJob != nil { break } // the job is skipped: nothing after this step runs
            seq += 1
        }
        return JobOutcome.decide(results, cancelled: cancelled || l.cancel.token.isCancelled, policy: OutcomePolicy())
    }

    /// A job that ends while it still holds its drive (it stopped before its Release step: failed, skipped, cancelled) runs
    /// the Release step once more, with its outcome as the context's finishing: the step decides whether the disc comes out
    /// (owner, 2026-10-18). Nothing is stored for it but its log lines; what it throws is ignored.
    private func releaseAtEnd(_ l: Live, _ decided: Decided) async {
        guard let record = l.loaded, l.logRead, let release = registry.step(.release),
              scheduler.leases().drives.contains(where: { $0.job == record.id }) else { return }
        let snapshot = JobSnapshot(id: record.id, request: record.request, plan: record.plan, outputs: l.outputs)
        var ending = makeContext(l, snapshot, l.seq)
        ending.progress = Silent()
        ending.finishing = decided
        let context = ending
        _ = try? await executor.worker { () async throws(BroError) -> StepReport in try await release.run(context) }
        try? await writeLog(l)
    }

    private func finish(_ l: Live, _ decided: Decided) async {
        let jobId = l.record.id
        l.progress = nil
        l.pendingProgress = nil
        l.blockedBy = nil
        l.decision = nil
        var record = l.record
        record.state = .finished
        record.outcome = decided.outcome
        record.error = decided.error
        record.finishedAt = clock.now()
        l.record = record
        let store = store, finished = record
        // The job has ended either way; what the store couldn't keep, recovery will see as unfinished.
        try? await executor.worker { () async throws(BroError) in try await store.jobs().update(finished) }
        coalescer?.drop(jobId.value)
        scheduler.finish(jobId)
        bus.publish(.jobFinished, data: .object([("job", JobDto.json(dto(l)))]))
    }

    // MARK: State and events

    /// Stores the record, with the job's place in its queue as it is now (JobService stores it when it changes; a stale one
    /// here would undo that).
    private func save(_ l: Live, _ record: JobRecord) async throws(BroError) {
        var placed = record
        placed.position = scheduler.position(record.id) ?? record.position
        let record = placed
        l.record = record
        let store = store
        try await executor.worker { () async throws(BroError) in try await store.jobs().update(record) }
    }

    private func setState(_ l: Live, _ state: JobState) {
        l.record.state = state
        bus.publish(.jobChanged, data: .object([("job", JobDto.json(dto(l)))]))
    }

    private func onWait(_ l: Live, _ wait: Wait) {
        switch wait {
        case let .blocked(reason):
            l.blockedBy = BroMessage(MessageCode(rawValue: reason.code) ?? .internalUnexpected, reason.params)
            setState(l, .blocked)
        case .waiting:
            l.blockedBy = nil
            setState(l, .waitingForResources)
        }
    }

    private func dto(_ l: Live) -> JobDto {
        var dto = JobDto.of(l.record)
        dto.progress = l.progress
        dto.blockedBy = l.blockedBy
        dto.decision = l.decision?.question
        return dto
    }

    fileprivate func onProgress(_ l: Live, _ seq: Int, _ fraction: Double, _ operation: BroMessage, _ current: String?) {
        guard l.seq == seq, l.record.state != .finished else { return }
        let f = min(max(fraction, 0), 1)
        l.pendingProgress = ProgressDto(step: l.steps[seq], stepIndex: seq, stepCount: l.steps.count,
                                        fraction: (Double(seq) + f) / Double(l.steps.count), currentFraction: f, operation: operation, current: current)
        coalescer?.offer(l.record.id.value, value: .null)
    }

    private func emitProgress(_ jobId: Id) {
        guard let l = live[jobId], let p = l.pendingProgress else { return }
        l.progress = p
        l.pendingProgress = nil
        setState(l, l.record.state)
    }

    fileprivate func onLog(_ l: Live, _ message: BroMessage) {
        l.logSeq += 1
        let line = LogLineDto(seq: l.logSeq, t: clock.now(), severity: message.severity, code: message.code, params: message.params)
        l.log.append(line)
        bus.publish(.jobLog, data: .object([("jobId", .string(l.record.id.value)), ("lines", .array([LogLineDto.json(line)]))]))
    }

    fileprivate func reserve(_ l: Live, _ volume: String, _ path: String, _ need: Int64, _ free: Int64) -> Result<Void, BroError> {
        do throws(BroError) {
            try scheduler.reserve(l.record.id, volume: volume, path: path, need: need, free: free)
            return .success(())
        } catch {
            return .failure(error)
        }
    }

    fileprivate func onAsk(_ l: Live, _ question: DecisionDto, _ answer: CheckedContinuation<Result<String, BroError>, Never>) {
        if l.cancel.token.isCancelled {
            answer.resume(returning: .failure(BroError(MessageCode.wire(.jobCancelled))))
            return
        }
        l.decision = (question, answer)
        setState(l, .awaitingDecision)
    }

    /// jobs/<id>/log.jsonl (JobLog): what was there when the job started, then this run's lines, written whole after each
    /// step.
    private func writeLog(_ l: Live) async throws(BroError) {
        if l.log.isEmpty { return }
        let directory = jobDirectory(l.record.id)
        let bytes = l.logBefore + JobLog.encode(l.log)
        let makeDirectory = !l.directoryMade
        l.directoryMade = true
        let fs = environment.fs
        try await executor.worker { () throws(BroError) in
            if makeDirectory { try fs.createDirectory(directory, parentsMustExist: false) }
            try fs.writeAtomically(directory + "/" + JobLog.fileName, bytes: bytes, mode: 0o600)
        }
    }

    private func jobDirectory(_ jobId: Id) -> String { environment.dataDirectory + "/jobs/" + jobId.value }

    private func makeContext(_ l: Live, _ snapshot: JobSnapshot, _ seq: Int) -> StepContext {
        StepContext(job: snapshot, cancel: l.cancel.token, progress: Reporter(runner: self, live: l, seq: seq, executor: executor),
                    log: Logger(runner: self, live: l, executor: executor), decisions: Decisions(runner: self, live: l, executor: executor),
                    tools: environment.tools, fs: environment.fs, clock: environment.clock, drives: environment.drives, library: environment.library,
                    catalog: environment.catalog, config: l.config!, driveLookup: environment.driveLookup, makemkvSetup: environment.makemkvSetup,
                    space: Space(runner: self, live: l, executor: executor), jobDirectory: jobDirectory(l.record.id))
    }

    static func unexpected(_ detail: String) -> BroError {
        BroError(MessageCode.wire(.internalUnexpected), [("detail", .string(detail))])
    }
}

// MARK: What a step's context hands back to the executor

private struct Reporter: ProgressReporter {
    let runner: PipelineRunner
    let live: PipelineRunner.Live
    let seq: Int
    let executor: EngineExecutor

    func report(_ fraction: Double, operation: BroMessage, current: String?) {
        executor.post { [runner, live, seq] in runner.onProgress(live, seq, fraction, operation, current) }
    }
}

private struct Logger: JobLogger {
    let runner: PipelineRunner
    let live: PipelineRunner.Live
    let executor: EngineExecutor

    func log(_ message: BroMessage) {
        executor.post { [runner, live] in runner.onLog(live, message) }
    }
}

private struct Silent: ProgressReporter {
    func report(_ fraction: Double, operation: BroMessage, current: String?) {}
}

private struct Space: SpaceReservations {
    let runner: PipelineRunner
    let live: PipelineRunner.Live
    let executor: EngineExecutor

    func reserve(_ volume: String, path: String, need: Int64, free: Int64) async throws(BroError) {
        let result = await withCheckedContinuation { (done: CheckedContinuation<Result<Void, BroError>, Never>) in
            let posted = executor.post { [runner, live] in done.resume(returning: runner.reserve(live, volume, path, need, free)) }
            if !posted { done.resume(returning: .failure(BroError(MessageCode.wire(.jobCancelled)))) }
        }
        try result.get()
    }
}

private struct Decisions: DecisionPort {
    let runner: PipelineRunner
    let live: PipelineRunner.Live
    let executor: EngineExecutor

    func ask(_ decision: DecisionDto) async throws(BroError) -> String {
        let result = await withCheckedContinuation { (answer: CheckedContinuation<Result<String, BroError>, Never>) in
            let posted = executor.post { [runner, live] in runner.onAsk(live, decision, answer) }
            if !posted { answer.resume(returning: .failure(BroError(MessageCode.wire(.jobCancelled)))) }
        }
        return try result.get()
    }
}
