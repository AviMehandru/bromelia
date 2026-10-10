import BroEngine
import BroFoundation
import BroTestSupport
import Foundation
import Testing

@EngineActor
final class Log {
    var items: [String] = []
    var count = 0
    var running = 0
    var overlapped = false
}

/// shared/fixtures/engine/engine-executor.cases.json: each case checked here. Swift has no isCurrent: the actions'
/// isolation to EngineActor is the compiler's (and EngineActor.assertIsolated's) job, and "one thread" is checked as
/// "never two actions at once".
struct EngineExecutorTests {
    /// Waits on a semaphore off the cooperative pool's caller (blocking is the point of these probes).
    static func wait(_ s: DispatchSemaphore, _ seconds: Double = 10) -> Bool { s.wait(timeout: .now() + seconds) == .success }

    static func check(_ id: String, _ given: JsonValue, _ expect: JsonValue) async throws {
        let exec = EngineExecutor()
        let log = Log()
        defer { Task { await exec.stop() } }
        switch id {
        case "in-order":
            let n = Int(given["actions"]!.int!)
            for i in 0..<n { exec.post { log.items.append("\(i)") } }
            await exec.barrier()
            try Fixtures.same((0..<n).map { "\($0)" }, await log.items, "order")
        case "in-order-per-thread":
            let threads = Int(given["threads"]!.int!), each = Int(given["actionsEach"]!.int!)
            let done = DispatchSemaphore(value: 0)
            for t in 0..<threads {
                Thread {
                    for i in 0..<each { exec.post { log.items.append("\(t) \(i)") } }
                    done.signal()
                }.start()
            }
            for _ in 0..<threads { try Fixtures.check(wait(done), "posting threads finished") }
            await exec.barrier()
            let items = await log.items
            try Fixtures.same(expect["ran"]!.int!, Int64(items.count), "ran")
            for t in 0..<threads {
                try Fixtures.same((0..<each).map { "\(t) \($0)" }, items.filter { $0.hasPrefix("\(t) ") }, "thread \(t)")
            }
        case "one-thread-of-its-own":
            for _ in 0..<Int(given["actions"]!.int!) {
                exec.post {
                    EngineActor.shared.assertIsolated()
                    log.running += 1
                    if log.running > 1 { log.overlapped = true }
                    log.count += 1
                    log.running -= 1
                }
            }
            await exec.barrier()
            try Fixtures.check(!(await log.overlapped), "never two actions at once")
            try Fixtures.same(given["actions"]!.int!, Int64(await log.count), "ran")
        case "posted-on-the-executor-runs-later":
            let busy = DispatchSemaphore(value: 0)
            exec.post { _ = wait(busy) }
            exec.post {
                log.items.append("first")
                exec.post { log.items.append("posted by first") }
                log.items.append("first ends")
            }
            exec.post { log.items.append("second") }
            busy.signal()
            await exec.barrier()
            await exec.barrier()
            try Fixtures.same(expect["order"]!.array!.map { $0.string! }, await log.items, "order")
        case "worker-runs-off-the-executor":
            // The work waits for an action posted to the executor: it can only run when the work isn't on it.
            let reached = DispatchSemaphore(value: 0)
            let result = try await workerThatWaitsForTheExecutor(exec, reached)
            try Fixtures.same(expect["result"]!.int!, result, "result (the executor ran while the work waited)")
        case "worker-error":
            let want = given["error"]!
            let error = BroError(want["code"]!.string!, want["params"]!.members!)
            let got = await Task { @EngineActor () -> BroError? in
                do throws(BroError) {
                    _ = try await exec.worker { () throws(BroError) -> Int in throw error }
                    return nil
                } catch {
                    EngineActor.shared.assertIsolated()
                    return error
                }
            }.value
            try Fixtures.same(expect["error"]!["code"]!.string!, got?.code ?? "none", "code")
            try Fixtures.same(expect["error"]!["params"]!, JsonValue.object(got?.params ?? []), "params")
        case "workers-in-parallel":
            let n = Int(given["workers"]!.int!)
            let started = Box(0), lock = NSLock(), all = DispatchSemaphore(value: 0)
            let results = await parallelWorkers(exec, n, started, lock, all)
            try Fixtures.check(results.allSatisfy { $0 }, "all started before any ended")
        case "barrier-waits":
            let ms = UInt32(given["slowActionMs"]!.int!)
            exec.post {
                usleep(ms * 1000)
                log.items.append("slow done")
            }
            await exec.barrier()
            try Fixtures.same(["slow done"], await log.items, "slow action done first")
        case "stop-runs-what-is-queued":
            let hold = DispatchSemaphore(value: 0)
            exec.post { _ = wait(hold) }
            for _ in 0..<Int(given["actions"]!.int!) { exec.post { log.count += 1 } }
            let stopping = Task { await exec.stop() }
            while exec.post({}) { try await Task.sleep(nanoseconds: 1_000_000) }
            hold.signal()
            await stopping.value
            let before = await log.count
            let accepted = exec.post { log.count += 1 }
            try await Task.sleep(nanoseconds: 20_000_000)
            try Fixtures.same(expect["ranBeforeStop"]!.int!, Int64(before), "ran before the stop ended")
            try Fixtures.same(expect["postAfterStop"]!.bool!, accepted, "post after stop")
            try Fixtures.same(expect["ranAfterStop"]!.int!, Int64(await log.count - before), "ran after stop")
        default:
            throw FixtureError("no test handles this case")
        }
    }

    @EngineActor
    static func workerThatWaitsForTheExecutor(_ exec: EngineExecutor, _ reached: DispatchSemaphore) async throws(BroError) -> Int64 {
        try await exec.worker { () throws(BroError) -> Int64 in
            exec.post { reached.signal() }
            return wait(reached, 5) ? 42 : -1
        }
    }

    @EngineActor
    static func parallelWorkers(_ exec: EngineExecutor, _ n: Int, _ started: Box<Int>, _ lock: NSLock, _ all: DispatchSemaphore) async -> [Bool] {
        var tasks: [Task<Bool, Never>] = []
        for _ in 0..<n {
            tasks.append(Task { @EngineActor in
                (try? await exec.worker { () throws(BroError) -> Bool in
                    let last = lock.withLock { started.value += 1; return started.value == n }
                    if last { for _ in 0..<n { all.signal() } }
                    return wait(all, 5)
                }) ?? false
            })
        }
        var out: [Bool] = []
        for t in tasks { out.append(await t.value) }
        return out
    }

    @Test func theSharedCasesPass() async throws {
        let doc = try Fixtures.json("engine/engine-executor.cases.json")
        var failures: [String] = []
        for c in doc["cases"]!.array! {
            let id = c["id"]!.string!
            do { try await EngineExecutorTests.check(id, c["given"]!, c["expect"]!) } catch { failures.append("\(id): \(error)") }
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }
}
