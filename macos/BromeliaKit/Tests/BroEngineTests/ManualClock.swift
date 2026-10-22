import BroDomain
import BroFoundation
import BroPorts
import Foundation

/// A clock that moves only when told (the scenarios' fake clock): it starts at 2026-10-02T20:00:00.000Z, and
/// `advance` fires each due timer in due order (ties in the order they were made), with the time set to its due time,
/// then awaits `settle`. Its local time is UTC plus `utcOffsetMinutes` (0 unless set; no daylight saving time). Safe
/// to use from any thread.
final class ManualClock: Clock, @unchecked Sendable {
    static let startMs: Int64 = 1_790_971_200_000

    private final class Pending: TimerHandle, @unchecked Sendable {
        let lock = NSLock()
        var dueMs: Int64 = 0
        var intervalMs: Int64 = 0
        var order: Int64 = 0
        var handler: @Sendable () -> Void = {}
        var cancelledFlag = false
        var cancelled: Bool { lock.withLock { cancelledFlag } }
        func cancel() { lock.withLock { cancelledFlag = true } }
    }

    private let lock = NSLock()
    private var nowMs = ManualClock.startMs
    private var timers: [Pending] = []
    private var made: Int64 = 0
    private var offsetMinutes = 0

    /// The local time's fixed offset from UTC.
    var utcOffsetMinutes: Int {
        get { lock.withLock { offsetMinutes } }
        set { lock.withLock { offsetMinutes = newValue } }
    }

    func now() -> Instant { lock.withLock { Instant(unixMilliseconds: nowMs) } }

    func monotonic() -> Duration { lock.withLock { Duration(seconds: Double(nowMs - ManualClock.startMs) / 1000) } }

    func sleep(_ duration: Duration, cancel: CancellationToken) async throws(BroError) {
        fatalError("not used by the engine")
    }

    func localTime(_ instant: Instant) -> LocalTime {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = TimeZone(secondsFromGMT: utcOffsetMinutes * 60)!
        let seconds = instant.unixMilliseconds >= 0 ? instant.unixMilliseconds / 1000 : (instant.unixMilliseconds - 999) / 1000
        let c = calendar.dateComponents([.year, .month, .day, .hour, .minute, .second], from: Date(timeIntervalSince1970: Double(seconds)))
        return LocalTime(year: c.year!, month: c.month!, day: c.day!, hour: c.hour!, minute: c.minute!, second: c.second!)
    }

    func timer(_ schedule: TimerSchedule, handler: @escaping @Sendable () -> Void) -> any TimerHandle {
        lock.withLock {
            let t = Pending()
            t.handler = handler
            t.order = made
            made += 1
            switch schedule {
            case let .at(instant): t.dueMs = instant.unixMilliseconds
            case let .every(interval):
                t.intervalMs = max(1, Int64((interval.seconds * 1000).rounded()))
                t.dueMs = nowMs + t.intervalMs
            }
            timers.append(t)
            return t
        }
    }

    /// The next due timer's handler, with the time moved to its due time; nil (and the time at target) when none.
    private func nextDue(before target: Int64) -> (@Sendable () -> Void)? {
        lock.withLock {
            timers.removeAll { $0.cancelled }
            guard let next = timers.filter({ $0.dueMs <= target }).min(by: { ($0.dueMs, $0.order) < ($1.dueMs, $1.order) }) else {
                nowMs = target
                return nil
            }
            nowMs = max(nowMs, next.dueMs)
            if next.intervalMs > 0 { next.dueMs += next.intervalMs } else { timers.removeAll { $0 === next } }
            return next.handler
        }
    }

    /// Fires the due timers, awaiting `settle` after each.
    func advance(_ ms: Int64, settle: () async -> Void) async {
        let target = lock.withLock { nowMs + ms }
        while let handler = nextDue(before: target) {
            handler()
            await settle()
        }
    }

    /// Fires the due timers without waiting for anything (on the executor).
    func advanceNow(_ ms: Int64) {
        let target = lock.withLock { nowMs + ms }
        while let handler = nextDue(before: target) { handler() }
    }
}

final class Box<T>: @unchecked Sendable {
    var value: T
    init(_ value: T) { self.value = value }
}
