import BroAdapters
import BroDomain
import BroFoundation
import BroPorts
import Foundation
import Testing

struct SystemClockTests {
    let clock = SystemClock()

    @Test func nowIsTheWallClock() {
        let wall = Int64(Date().timeIntervalSince1970 * 1000)
        #expect(abs(clock.now().unixMilliseconds - wall) < 1000)
    }

    @Test func sleepsAndTheMonotonicClockAdvances() async throws {
        let before = clock.monotonic()
        try await clock.sleep(Duration(seconds: 0.2), cancel: CancellationToken())
        let slept = clock.monotonic().seconds - before.seconds
        #expect(slept >= 0.15 && slept < 5)
    }

    @Test func aCancelledSleepFailsWithJobCancelled() async {
        let cancel = CancellationToken()
        let sleeper = Task { () async -> String? in
            do {
                try await clock.sleep(Duration(seconds: 30), cancel: cancel)
                return nil
            } catch let error as BroError {
                return error.code
            } catch {
                return "\(error)"
            }
        }
        try? await Task.sleep(nanoseconds: 50_000_000)
        cancel.cancel()
        #expect(await sleeper.value == "job.cancelled")
    }

    @Test func timersFireOnceOrRepeatedlyUntilCancelled() async throws {
        let fired = Counter()
        _ = clock.timer(.at(instant: Instant(unixMilliseconds: clock.now().unixMilliseconds + 100))) { fired.add() }
        for _ in 0..<100 where fired.value == 0 { try await Task.sleep(nanoseconds: 50_000_000) }
        #expect(fired.value == 1)

        let ticks = Counter()
        let every = clock.timer(.every(interval: Duration(seconds: 0.05))) { ticks.add() }
        for _ in 0..<100 where ticks.value < 3 { try await Task.sleep(nanoseconds: 50_000_000) }
        every.cancel()
        #expect(ticks.value >= 3)
        try await Task.sleep(nanoseconds: 100_000_000)
        let after = ticks.value
        try await Task.sleep(nanoseconds: 200_000_000)
        #expect(ticks.value == after)
    }
}

final class Counter: @unchecked Sendable {
    private let lock = NSLock()
    private var n = 0
    func add() { lock.withLock { n += 1 } }
    var value: Int { lock.withLock { n } }
}
