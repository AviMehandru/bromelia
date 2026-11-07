import BroDomain
import BroFoundation

/// The three queues, the resources steps ask for, and space reservations (plan §21), on the executor.
/// shared/fixtures/engine/scheduler.cases.json.
///
/// - A drive has room for one job, and a job keeps a drive it was given until `releaseDrives` or `finish`; a step's
///   other resources go back on `release`.
/// - Acquisition slots: limits.acquisitions (0 = no cap; a job put first by `startNow` isn't held back by it).
///   Library writes: unlimited, blocked while the library is offline. Cpu: limits.cpu. Io per volume:
///   limits.ioPerVolume. Every request of a job in the processing queue also takes a processing slot
///   (limits.processing) while its step runs. A maintenance job waits while any acquisition slot is held.
/// - Requests are considered by queue, then the job's place in it, then request order. A request that has to wait
///   keeps later ones from overtaking it only on the resources it waits for.
/// - A job in a queue that hasn't asked for anything yet (its run is still loading) holds its place: nothing is
///   granted to a job behind it in that queue until it asks or finishes. Those held back hear nothing.
@EngineActor
public final class Scheduler {
    private static let acquisitionKey = "acquisition"
    private static let processingKey = "processing"
    private static let cpuKey = "cpu"

    @EngineActor
    private final class Request {
        let job: Id
        let resources: ResourceSet
        let onWait: @EngineActor (Wait) -> Void
        let done: Resumption<Result<Lease, BroError>>
        let order: Int
        var lastWait: Wait?
        var unregister: (@Sendable () -> Void)?

        init(job: Id, resources: ResourceSet, onWait: @escaping @EngineActor (Wait) -> Void,
             done: Resumption<Result<Lease, BroError>>, order: Int) {
            self.job = job
            self.resources = resources
            self.onWait = onWait
            self.done = done
            self.order = order
        }
    }

    private struct Reservation: Hashable {
        let job: Id
        let volume: String
    }

    private let executor: EngineExecutor
    private let bus: EventBus
    private var limits: SchedulerLimits
    private var queueLists: [Queue: [Id]] = [.acquisition: [], .processing: [], .maintenance: []]
    private var queueOf: [Id: Queue] = [:]
    private var startNowJobs: Set<Id> = []
    /// Jobs that have asked for something since they were submitted.
    private var requestedJobs: Set<Id> = []
    private var pending: [Request] = []
    private var driveHolder: [String: Id] = [:]
    private var held: [String: Int] = [:]
    private var leasesOf: [Id: [Lease]] = [:]
    private var offline: [String: BroError] = [:]
    private var reserved: [Reservation: Int64] = [:]
    private var published = LeaseSnapshot.empty
    private var nextOrder = 0
    private var granted: Int64 = 0

    public init(executor: EngineExecutor, bus: EventBus, limits: SchedulerLimits) {
        self.executor = executor
        self.bus = bus
        self.limits = limits
    }

    // MARK: Queues

    public func submit(_ jobId: Id, queue: Queue) {
        if queueOf[jobId] != nil { return }
        queueOf[jobId] = queue
        queueLists[queue]!.append(jobId)
        publishQueue(queue)
        pump()
    }

    /// Moves a job to the end of another queue (after Release: processing).
    public func moveTo(_ jobId: Id, queue: Queue) {
        guard let from = queueOf[jobId], from != queue else { return }
        queueLists[from]!.removeAll { $0 == jobId }
        queueOf[jobId] = queue
        queueLists[queue]!.append(jobId)
        publishQueue(from)
        publishQueue(queue)
        pump()
    }

    /// Moves a job within its queue, stopping at the ends.
    public func move(_ jobId: Id, by: Int) throws(BroError) {
        let queue = try queueOfOrFail(jobId)
        var order = queueLists[queue]!
        let from = order.firstIndex(of: jobId)!
        let to = min(max(from + by, 0), order.count - 1)
        if from == to { return }
        order.remove(at: from)
        order.insert(jobId, at: to)
        queueLists[queue] = order
        publishQueue(queue)
        pump()
    }

    /// Puts a job first in its queue; from now on the acquisitions cap doesn't hold it back.
    public func startNow(_ jobId: Id) throws(BroError) {
        let queue = try queueOfOrFail(jobId)
        startNowJobs.insert(jobId)
        if queueLists[queue]!.first != jobId {
            queueLists[queue]!.removeAll { $0 == jobId }
            queueLists[queue]!.insert(jobId, at: 0)
            publishQueue(queue)
        }
        pump()
    }

    private func queueOfOrFail(_ jobId: Id) throws(BroError) -> Queue {
        guard let queue = queueOf[jobId] else {
            throw BroError(MessageCode.wire(.jobNotQueued), [("jobId", .string(jobId.value))])
        }
        return queue
    }

    /// Cancels the job's waiting requests, gives back everything it holds and its space, and takes it out of its
    /// queue.
    public func finish(_ jobId: Id) {
        for r in pending where r.job == jobId { cancelRequest(r) }
        for lease in leasesOf.removeValue(forKey: jobId) ?? [] { giveBack(lease) }
        driveHolder = driveHolder.filter { $0.value != jobId }
        unreserve(jobId)
        startNowJobs.remove(jobId)
        requestedJobs.remove(jobId)
        if let queue = queueOf.removeValue(forKey: jobId) {
            queueLists[queue]!.removeAll { $0 == jobId }
            publishQueue(queue)
        }
        pump()
    }

    /// The job's place in its queue (0 is first); nil when it is in none.
    public func position(_ jobId: Id) -> Int? {
        guard let q = queueOf[jobId] else { return nil }
        return queueLists[q]!.firstIndex(of: jobId)
    }

    public func queues() -> QueueOrders {
        QueueOrders(acquisition: queueLists[.acquisition]!, processing: queueLists[.processing]!, maintenance: queueLists[.maintenance]!)
    }

    // MARK: Leases

    /// Asks for `resources`; granted, never inside the call, when everything has room. `onWait` hears each change
    /// while the request waits. Fails with job.cancelled when cancelled (or the job finishes) first, and with
    /// job.notQueued for a job in no queue.
    public func acquire(_ jobId: Id, resources: ResourceSet, onWait: @escaping @EngineActor (Wait) -> Void,
                        cancel: CancellationToken) async throws(BroError) -> Lease {
        _ = try queueOfOrFail(jobId)
        let result = await executor.suspend { (done: Resumption<Result<Lease, BroError>>) in
            let request = Request(job: jobId, resources: resources, onWait: onWait, done: done, order: nextOrder)
            nextOrder += 1
            requestedJobs.insert(jobId)
            pending.append(request)
            let executor = executor
            request.unregister = cancel.onCancel { [weak self, weak request] in
                executor.post {
                    guard let self, let request, self.pending.contains(where: { $0 === request }) else { return }
                    self.cancelRequest(request)
                    self.pump()
                }
            }
            pump()
        }
        return try result.get()
    }

    private func cancelRequest(_ request: Request) {
        pending.removeAll { $0 === request }
        request.unregister?()
        request.done.resume(returning: .failure(BroError(MessageCode.wire(.jobCancelled))))
    }

    /// Cancels every waiting request of the jobs in `queue`.
    public func cancelAll(_ queue: Queue) {
        for r in pending where queueOf[r.job] == queue { cancelRequest(r) }
        pump()
    }

    /// Gives back a lease, but not its drives. Twice does nothing.
    public func release(_ lease: Lease) {
        if lease.released { return }
        giveBack(lease)
        leasesOf[lease.jobId]?.removeAll { $0 === lease }
        if leasesOf[lease.jobId]?.isEmpty == true { leasesOf.removeValue(forKey: lease.jobId) }
        pump()
    }

    private func giveBack(_ lease: Lease) {
        lease.released = true
        for (key, n) in lease.counted { held[key, default: 0] -= n }
    }

    /// Gives back the job's drives (the Release step).
    public func releaseDrives(_ jobId: Id) {
        driveHolder = driveHolder.filter { $0.value != jobId }
        pump()
    }

    public func setLibraryOffline(_ libraryId: String, reason: BroError?) {
        offline[libraryId] = reason
        pump()
    }

    public func updateLimits(_ limits: SchedulerLimits) {
        self.limits = limits
        pump()
    }

    public func leases() -> LeaseSnapshot {
        let drives = driveHolder.sorted { Array($0.key.utf8).lexicographicallyPrecedes(Array($1.key.utf8)) }
            .map { DriveHolder(drive: $0.key, job: $0.value) }
        let holding = Set(leasesOf.keys).union(driveHolder.values).filter { queueOf[$0] != nil }
        func count(_ q: Queue) -> Int { holding.filter { queueOf[$0] == q }.count }
        return LeaseSnapshot(drives: drives, acquisitions: held[Scheduler.acquisitionKey] ?? 0,
                             jobs: JobCounts(acquisition: count(.acquisition), processing: count(.processing), maintenance: count(.maintenance)))
    }

    // MARK: Space

    /// Keeps `need` bytes on `volume` for the job, replacing what it kept there. space.insufficient when `free` minus
    /// what other jobs keep there is less.
    public func reserve(_ jobId: Id, volume: String, path: String, need: Int64, free: Int64) throws(BroError) {
        let others = reserved.filter { $0.key.volume == volume && $0.key.job != jobId }.values.reduce(0, +)
        let available = max(0, free - others)
        if available < need {
            throw BroError(MessageCode.wire(.spaceInsufficient),
                           [("path", .string(path)), ("need", .integer(need)), ("free", .integer(available))])
        }
        reserved[Reservation(job: jobId, volume: volume)] = need
    }

    public func unreserve(_ jobId: Id) {
        reserved = reserved.filter { $0.key.job != jobId }
    }

    // MARK: Granting

    private static func rank(_ q: Queue) -> Int {
        switch q {
        case .acquisition: return 0
        case .processing: return 1
        case .maintenance: return 2
        }
    }

    private func capacity(_ key: String, _ job: Id) -> Int {
        switch key {
        case Scheduler.acquisitionKey:
            return limits.acquisitions == 0 || startNowJobs.contains(job) ? Int.max : limits.acquisitions
        case Scheduler.processingKey: return max(1, limits.processing)
        case Scheduler.cpuKey: return max(1, limits.cpu)
        default: return max(1, limits.ioPerVolume) // io:<volume>
        }
    }

    private func counted(_ r: Request) -> [(key: String, count: Int)] {
        var needs: [(key: String, count: Int)] = []
        if r.resources.acquisition { needs.append((Scheduler.acquisitionKey, 1)) }
        if queueOf[r.job] == .processing { needs.append((Scheduler.processingKey, 1)) }
        if r.resources.cpu > 0 { needs.append((Scheduler.cpuKey, min(r.resources.cpu, capacity(Scheduler.cpuKey, r.job)))) }
        var seen: Set<String> = []
        for v in r.resources.io where seen.insert(v).inserted { needs.append(("io:" + v, 1)) }
        return needs
    }

    private func pump() {
        var claimed = held
        var waitedDrives: Set<String> = []
        // Each queue's first job that hasn't asked yet: the jobs behind it wait for it, without a word.
        let holdFrom = queueLists.mapValues { $0.firstIndex { !requestedJobs.contains($0) } }
        let order = pending.sorted { a, b in
            let qa = queueOf[a.job]!, qb = queueOf[b.job]!
            if Scheduler.rank(qa) != Scheduler.rank(qb) { return Scheduler.rank(qa) < Scheduler.rank(qb) }
            let ia = queueLists[qa]!.firstIndex(of: a.job)!, ib = queueLists[qb]!.firstIndex(of: b.job)!
            return ia != ib ? ia < ib : a.order < b.order
        }
        for r in order {
            let queue = queueOf[r.job]!
            if let first = holdFrom[queue]!, queueLists[queue]!.firstIndex(of: r.job)! > first { continue }
            let needs = counted(r)
            var seenDrives: Set<String> = []
            let waitDrives = r.resources.drives.filter { d in
                guard seenDrives.insert(d).inserted else { return false }
                let holder = driveHolder[d]
                return (holder != nil && holder != r.job) || (waitedDrives.contains(d) && holder != r.job)
            }
            let waitKeys = needs.filter { (claimed[$0.key] ?? 0) + $0.count > capacity($0.key, r.job) }
            let gated = queueOf[r.job] == .maintenance && (held[Scheduler.acquisitionKey] ?? 0) > 0
            let blocked = r.resources.libraryWrites.lazy.compactMap { self.offline[$0] }.first
            if blocked == nil && !gated && waitDrives.isEmpty && waitKeys.isEmpty {
                grant(r, needs)
                for (key, n) in needs { claimed[key, default: 0] += n }
                continue
            }
            waitedDrives.formUnion(waitDrives)
            for (key, n) in waitKeys { claimed[key, default: 0] += n }
            let wait: Wait = blocked.map { .blocked($0) } ?? .waiting
            if wait != r.lastWait {
                r.lastWait = wait
                r.onWait(wait)
            }
        }
        let snapshot = leases()
        if snapshot != published {
            published = snapshot
            bus.publish(.leaseChanged, data: LeaseSnapshot.json(snapshot))
        }
    }

    private func grant(_ r: Request, _ needs: [(key: String, count: Int)]) {
        pending.removeAll { $0 === r }
        r.unregister?()
        for d in r.resources.drives { driveHolder[d] = r.job }
        for (key, n) in needs { held[key, default: 0] += n }
        granted += 1
        let lease = Lease(jobId: r.job, seq: granted, counted: needs)
        leasesOf[r.job, default: []].append(lease)
        r.done.resume(returning: .success(lease))
    }

    private func publishQueue(_ queue: Queue) {
        bus.publish(.queueChanged, data: QueueChange.json(QueueChange(queue: queue, order: queueLists[queue]!)))
    }
}
