import BroAdapters
import BroDomain
import BroFoundation
import BroPorts
import Foundation
import Testing
#if canImport(Glibc)
import Glibc
#endif

/// The fake clocks' local time: `instant` in UTC, cut to the second.
func utcLocalTime(_ instant: Instant) -> LocalTime {
    var calendar = Calendar(identifier: .gregorian)
    calendar.timeZone = TimeZone(secondsFromGMT: 0)!
    let c = calendar.dateComponents([.year, .month, .day, .hour, .minute, .second],
                                    from: Date(timeIntervalSince1970: Double(instant.unixMilliseconds / 1000)))
    return LocalTime(year: c.year!, month: c.month!, day: c.day!, hour: c.hour!, minute: c.minute!, second: c.second!)
}

struct SystemClockTests {
    let clock = SystemClock()

    @Test func nowIsTheWallClock() {
        let wall = Int64(Date().timeIntervalSince1970 * 1000)
        #expect(abs(clock.now().unixMilliseconds - wall) < 1000)
    }

    /// Winter and summer (daylight saving time either side of the equator), a time cut to the second, now; the OS's
    /// answer is libc's localtime_r.
    @Test func localTimeIsTheOperatingSystemsOwn() {
        for at in ["2026-01-15T12:00:00.000Z", "2026-07-15T12:00:00.000Z", "1999-12-31T23:59:59.999Z", Instant.format(clock.now())] {
            let instant = Instant.parse(at)!
            var seconds = time_t(instant.unixMilliseconds / 1000)
            var parts = tm()
            localtime_r(&seconds, &parts)
            let os = LocalTime(year: Int(parts.tm_year) + 1900, month: Int(parts.tm_mon) + 1, day: Int(parts.tm_mday),
                               hour: Int(parts.tm_hour), minute: Int(parts.tm_min), second: Int(parts.tm_sec))
            #expect(clock.localTime(instant) == os, "\(at)")
        }
    }

    @Test func sleepsAndTheMonotonicClockAdvances() async throws {
        let before = clock.monotonic()
        try await clock.sleep(Duration(seconds: 0.2), cancel: CancellationSource().token)
        let slept = clock.monotonic().seconds - before.seconds
        #expect(slept >= 0.15 && slept < 5)
    }

    @Test func aCancelledSleepFailsWithJobCancelled() async {
        let cancelSource = CancellationSource()
        let cancel = cancelSource.token
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
        cancelSource.cancel()
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
