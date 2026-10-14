import BroDomain
import BroEngine
import BroFoundation
import BroTestSupport
import Foundation
import Testing

@EngineActor
final class SchedulerHarness {
    let world = World()
    let bus: EventBus
    let scheduler: Scheduler
    var jobs: [String: Id] = [:]
    var granted: [(name: String, seq: Int64)] = []
    var states: [String: String] = [:]
    var waits: [String: [String]] = [:]
    var leases: [String: Lease] = [:]
    var cancels: [String: CancellationSource] = [:]
    var events: [EngineEvent] = []
    var started: Set<String> = []
    var subscription: Subscription?

    init(_ given: JsonValue) {
        bus = EventBus(clock: world.clock)
        scheduler = Scheduler(executor: world.executor, bus: bus, limits: SchedulerHarness.limits(given["limits"]))
        subscription = bus.subscribe(types: []) { [unowned self] e in self.events.append(e) }
    }

    nonisolated static func limits(_ j: JsonValue?) -> SchedulerLimits {
        SchedulerLimits(acquisitions: Int(j?["acquisitions"]?.int ?? 0), processing: Int(j?["processing"]?.int ?? 1),
                        cpu: Int(j?["cpu"]?.int ?? 2), ioPerVolume: Int(j?["ioPerVolume"]?.int ?? 2))
    }

    nonisolated static func strings(_ j: JsonValue?) -> [String] { (j?.array ?? []).map { $0.string! } }

    func job(_ name: String) -> Id {
        if let id = jobs[name] { return id }
        let hex = String(jobs.count + 1, radix: 16)
        let id = Id("00000000-0000-4000-8000-" + String(repeating: "0", count: 12 - hex.count) + hex)
        jobs[name] = id
        return id
    }

    func name(of id: Id) -> String { jobs.first { $0.value == id }!.key }

    func expecting(_ expectError: JsonValue?, _ body: () throws(BroError) -> Void) throws {
        var failure: BroError?
        do throws(BroError) {
            try body()
        } catch {
            failure = error
        }
        switch (failure, expectError) {
        case (nil, nil): return
        case (nil, let want?): throw FixtureError("expected the error \(want)")
        case (let error?, nil): throw FixtureError("unexpected error \(error.code)")
        case let (error?, expectError?):
            if let code = expectError.string {
                try Fixtures.same(code, error.code, "error")
            } else {
                try Fixtures.same(expectError["code"]!.string!, error.code, "error")
                try Fixtures.same(expectError["params"]!, JsonValue.object(error.params), "error params")
            }
        }
    }

    func run(_ op: JsonValue) async throws {
        let (name, arg) = op.members!.first!
        switch name {
        case "submit":
            scheduler.submit(job(arg["job"]!.string!), queue: Queue(rawValue: arg["queue"]!.string!)!)
        case "request":
            let label = arg["as"]!.string!
            let resources = ResourceSet(drives: SchedulerHarness.strings(arg["drives"]), acquisition: arg["acquisition"]?.bool == true,
                                        libraryWrites: SchedulerHarness.strings(arg["libraryWrites"]), cpu: Int(arg["cpu"]?.int ?? 0),
                                        io: SchedulerHarness.strings(arg["io"]))
            let cancel = CancellationSource()
            cancels[label] = cancel
            waits[label] = []
            let jobId = job(arg["job"]!.string!)
            Task { @EngineActor in
                self.started.insert(label)
                do throws(BroError) {
                    let lease = try await self.scheduler.acquire(jobId, resources: resources, onWait: { wait in
                        let text: String
                        switch wait {
                        case .waiting: text = "waiting"
                        case let .blocked(reason): text = "blocked:" + reason.code
                        }
                        self.waits[label, default: []].append(text)
                        self.states[label] = text
                    }, cancel: cancel.token)
                    self.leases[label] = lease
                    self.granted.append((label, lease.seq))
                    self.states[label] = "granted"
                } catch {
                    self.states[label] = error.code == "job.cancelled" ? "cancelled" : "failed " + error.code
                }
            }
            while !started.contains(label) { await Task.yield() }
        case "release":
            scheduler.release(leases[arg.string!]!)
            states[arg.string!] = "released"
        case "releaseDrives":
            scheduler.releaseDrives(job(arg.string!))
        case "toProcessing":
            scheduler.moveTo(job(arg.string!), queue: .processing)
        case "finish":
            let id = job(arg.string!)
            for (n, lease) in leases where lease.jobId == id && states[n] == "granted" { states[n] = "released" }
            scheduler.finish(id)
        case "move":
            let id = job(arg["job"]!.string!), by = Int(arg["by"]!.int!)
            try expecting(arg["expectError"]) { () throws(BroError) in try scheduler.move(id, by: by) }
        case "startNow":
            let id = job(arg["job"]!.string!)
            try expecting(arg["expectError"]) { () throws(BroError) in try scheduler.startNow(id) }
        case "cancel":
            cancels[arg.string!]!.cancel()
        case "cancelAll":
            scheduler.cancelAll(Queue(rawValue: arg.string!)!)
        case "library":
            let id = arg["id"]!.string!
            scheduler.setLibraryOffline(id, reason: arg["online"]!.bool! ? nil
                                        : BroError(MessageCode.wire(.libraryOffline), [("library", .string(id))]))
        case "limits":
            scheduler.updateLimits(SchedulerHarness.limits(arg))
        case "reserve":
            let id = job(arg["job"]!.string!)
            try expecting(arg["expectError"]) { () throws(BroError) in
                try scheduler.reserve(id, volume: arg["volume"]!.string!, path: arg["path"]!.string!, need: arg["need"]!.int!, free: arg["free"]!.int!)
            }
        case "unreserve":
            scheduler.unreserve(job(arg.string!))
        default:
            throw FixtureError("unknown op \(name)")
        }
    }

    /// Nothing is left to run: no posted action, and no grant or cancellation whose request hasn't recorded it.
    /// A barrier isn't enough: the Scheduler wakes a waiting request through `EngineExecutor.suspend`, and Swift runs
    /// it again on the actor, outside the mailbox, so it can still be waiting after the barrier's action has run.
    nonisolated func settled() async -> Bool {
        await withCheckedContinuation { (done: CheckedContinuation<Bool, Never>) in
            // Posted alone (no barrier after it), so the executor's activity counts only the scheduler's own work.
            if !world.executor.post({ [world] in done.resume(returning: world.executor.activity == 0) }) { done.resume(returning: true) }
        }
    }

    nonisolated func settle() async throws {
        let until = Date().addingTimeInterval(10)
        while !(await settled()) {
            if Date() > until { throw FixtureError("the scheduler didn't settle") }
            try await Task.sleep(nanoseconds: 1_000_000)
        }
    }

    func queuesJson() -> JsonValue {
        let q = scheduler.queues()
        func names(_ ids: [Id]) -> JsonValue { .array(ids.map { .string(name(of: $0)) }) }
        return .object([("acquisition", names(q.acquisition)), ("processing", names(q.processing)), ("maintenance", names(q.maintenance))])
    }
}

/// shared/fixtures/engine/scheduler.cases.json, on a real executor.
struct SchedulerTests {
    @Test func theSharedCasesPass() async throws {
        let doc = try Fixtures.json("engine/scheduler.cases.json")
        var failures: [String] = []
        for c in doc["cases"]!.array! {
            let id = c["id"]!.string!, given = c["given"]!, expect = c["expect"]!
            let h = await SchedulerHarness(given)
            do {
                for op in given["ops"]!.array! {
                    try await h.run(op)
                    try await h.settle()
                }
                let (granted, states, waits, queues, leases, events) = await (h.granted.sorted { $0.seq < $1.seq }.map(\.name), h.states, h.waits, h.queuesJson(),
                                                                               LeaseSnapshot.json(h.scheduler.leases()),
                                                                               JsonValue.array(h.events.map(EngineEvent.json)))
                if let want = expect["granted"] { try Fixtures.same(SchedulerHarness.strings(want), granted, "granted") }
                for (name, state) in expect["states"]?.members ?? [] { try Fixtures.same(state.string, states[name], "state of \(name)") }
                for (name, w) in expect["waits"]?.members ?? [] { try Fixtures.same(SchedulerHarness.strings(w), waits[name] ?? [], "waits of \(name)") }
                if let want = expect["queues"] { try Fixtures.same(want, queues, "queues") }
                if let want = expect["leases"] { try Fixtures.same(want, leases, "leases") }
                if let want = expect["events"] { try Fixtures.same(want, events, "events") }
            } catch {
                failures.append("\(id): \(error)")
            }
            await h.world.executor.stop()
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    @Test func aLeaseSnapshotReadsBackAsWritten() {
        let snapshot = LeaseSnapshot(drives: [DriveHolder(drive: "drv-0123456789abcdef", job: Id("00000000-0000-4000-8000-000000000001"))],
                                     acquisitions: 1, jobs: JobCounts(acquisition: 1, processing: 2, maintenance: 3))
        #expect(LeaseSnapshot.parse(LeaseSnapshot.json(snapshot)) == snapshot)
        #expect(LeaseSnapshot.parse(.object([("drives", .integer(1))])) == nil)
    }
}
