import BroDomain
import BroEngine
import BroFoundation
import BroPorts
import BroTestKit
import BroTestSupport
import Testing

/// A real executor and an EngineClock over a manual clock that waits for the executor after each timer.
final class World: Sendable {
    let clock = ManualClock()
    let executor = EngineExecutor()
    let engineClock: EngineClock

    init() { engineClock = EngineClock(clock: clock, executor: executor) }

    /// Runs `action` on the executor and waits for it.
    func onExecutor(_ action: @escaping EngineExecutor.Action) async {
        executor.post(action)
        await executor.barrier()
    }

    func advance(_ ms: Int64) async {
        await clock.advance(ms) { await self.executor.barrier() }
    }
}

@EngineActor
final class ClockState {
    var timers: [String: EngineTimer] = [:]
    var fired: [String] = []
    var emitted: [String] = []
    var localTimes: [String] = []
    var coalescer: Coalescer?
}

/// shared/fixtures/engine/engine-clock.cases.json and coalescer.cases.json, on a real executor.
struct EngineClockTests {
    static func run(_ ops: [JsonValue], _ w: World, _ state: ClockState) async throws {
        for op in ops {
            guard let (name, arg) = op.members?.first else { throw FixtureError("bad op") }
            switch name {
            case "timer":
                let name = arg["as"]!.string!
                let schedule: TimerSchedule = arg["at"].map { .at(instant: Instant(unixMilliseconds: ManualClock.startMs + $0.int!)) }
                    ?? .every(interval: Duration(seconds: arg["every"]!.double!))
                let cancelAfter = arg["cancelSelfAfter"]?.int.map(Int.init)
                await w.onExecutor {
                    var count = 0
                    state.timers[name] = w.engineClock.timer(schedule) {
                        EngineActor.shared.assertIsolated()
                        state.fired.append(name)
                        count += 1
                        if count == cancelAfter { state.timers[name]!.cancel() }
                    }
                }
            case "advance":
                await w.advance(arg.int!)
            case "cancel":
                let name = arg.string!
                await w.onExecutor { state.timers[name]!.cancel() }
            case "onExecutor":
                let inner = arg.array!
                await w.onExecutor {
                    for op in inner {
                        let (name, arg) = op.members!.first!
                        switch name {
                        case "advance": w.clock.advanceNow(arg.int!)
                        case "cancel": state.timers[arg.string!]!.cancel()
                        default: fatalError("unknown op \(name) on the executor")
                        }
                    }
                }
            case "utcOffsetMinutes":
                w.clock.utcOffsetMinutes = Int(arg.int!)
            case "localTime":
                let t = w.engineClock.localTime(Instant.parse(arg.string!)!)
                let text = "\(pad(t.year, 4))-\(pad(t.month))-\(pad(t.day)) \(pad(t.hour)):\(pad(t.minute)):\(pad(t.second))"
                await w.onExecutor { state.localTimes.append(text) }
            default:
                throw FixtureError("unknown op \(name)")
            }
        }
    }

    static func pad(_ n: Int, _ width: Int = 2) -> String {
        let s = String(n)
        return String(repeating: "0", count: max(0, width - s.count)) + s
    }

    @Test func theSharedCasesPass() async throws {
        let doc = try Fixtures.json("engine/engine-clock.cases.json")
        var failures: [String] = []
        for c in doc["cases"]!.array! {
            let id = c["id"]!.string!, given = c["given"]!, expect = c["expect"]!
            let w = World()
            let state = ClockState()
            do {
                try await EngineClockTests.run(given["ops"]!.array!, w, state)
                await w.executor.barrier()
                let fired = await state.fired
                try Fixtures.same(expect["fired"]!.array!.map { $0.string! }, fired, "fired")
                if let now = expect["now"]?.string { try Fixtures.same(now, Instant.format(w.engineClock.now()), "now") }
                if let mono = expect["monotonicSeconds"]?.double { try Fixtures.same(mono, w.engineClock.monotonic().seconds, "monotonic") }
                let localTimes = await state.localTimes
                try Fixtures.same(expect["localTimes"]?.array?.map { $0.string! } ?? [], localTimes, "localTimes")
            } catch {
                failures.append("\(id): \(error)")
            }
            await w.executor.stop()
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    @Test func theSharedCoalescerCasesPass() async throws {
        let doc = try Fixtures.json("engine/coalescer.cases.json")
        var failures: [String] = []
        for c in doc["cases"]!.array! {
            let id = c["id"]!.string!, given = c["given"]!, expect = c["expect"]!
            let w = World()
            let state = ClockState()
            await w.onExecutor {
                state.coalescer = Coalescer(clock: w.engineClock, interval: Duration(seconds: 0.1)) { key, value in
                    let ms = Int64((w.engineClock.monotonic().seconds * 1000).rounded())
                    state.emitted.append("\(ms) \(key) \(value.string ?? "")")
                }
            }
            do {
                for op in given["ops"]!.array! {
                    let (name, arg) = op.members!.first!
                    switch name {
                    case "offer":
                        let key = arg["key"]!.string!, value = arg["value"]!
                        await w.onExecutor { state.coalescer!.offer(key, value: value) }
                    case "advance": await w.advance(arg.int!)
                    case "flush":
                        let key = arg.string!
                        await w.onExecutor { state.coalescer!.flush(key) }
                    case "drop":
                        let key = arg.string!
                        await w.onExecutor { state.coalescer!.drop(key) }
                    default: throw FixtureError("unknown op \(name)")
                    }
                }
                await w.executor.barrier()
                let emitted = await state.emitted
                try Fixtures.same(expect["emitted"]!.array!.map { $0.string! }, emitted, "emitted")
            } catch {
                failures.append("\(id): \(error)")
            }
            await w.executor.stop()
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }
}
