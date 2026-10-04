import BroDomain
import BroFoundation
import BroPorts
import Dispatch
import Foundation

/// The real clock: wall time, a monotonic clock, cancellable sleeps and timers.
public final class SystemClock: Clock {
    public init() {}

    public func now() -> Instant {
        Instant(unixMilliseconds: Int64((Date().timeIntervalSince1970 * 1000).rounded(.down)))
    }

    /// CLOCK_MONOTONIC, which keeps counting while the Mac sleeps.
    public func monotonic() -> Duration {
        Duration(seconds: Double(clock_gettime_nsec_np(CLOCK_MONOTONIC)) / 1e9)
    }

    public func sleep(_ duration: Duration, cancel: CancellationToken) async throws(BroError) {
        let woke = Once()
        let finished: Bool = await withCheckedContinuation { (c: CheckedContinuation<Bool, Never>) in
            let remove = cancel.onCancel { if woke.claim() { c.resume(returning: false) } }
            DispatchQueue.global().asyncAfter(deadline: .now() + max(0, duration.seconds)) {
                remove()
                if woke.claim() { c.resume(returning: true) }
            }
        }
        if !finished || cancel.isCancelled { throw BroError(MessageCode.jobCancelled.rawValue) }
    }

    /// A timer lives until it has fired (at) or is cancelled, whether or not the handle is kept.
    public func timer(_ schedule: TimerSchedule, handler: @escaping @Sendable () -> Void) -> any TimerHandle {
        let source = DispatchSource.makeTimerSource(queue: DispatchQueue.global())
        var once = false
        switch schedule {
        case .at(let instant):
            let ms = max(0, instant.unixMilliseconds - now().unixMilliseconds)
            source.schedule(deadline: .now() + .milliseconds(Int(ms)))
            once = true
        case .every(let interval):
            source.schedule(deadline: .now() + interval.seconds, repeating: interval.seconds)
        }
        // The handler holds the source, so it stays alive until cancelled; cancelling releases the handler.
        source.setEventHandler {
            handler()
            if once { source.cancel() }
        }
        source.resume()
        return Handle(source)
    }

    private final class Handle: TimerHandle, @unchecked Sendable {
        private let source: DispatchSourceTimer
        init(_ source: DispatchSourceTimer) { self.source = source }
        func cancel() { source.cancel() }
    }

    /// True for the first caller only.
    private final class Once: @unchecked Sendable {
        private let lock = NSLock()
        private var claimed = false
        func claim() -> Bool { lock.withLock { defer { claimed = true }; return !claimed } }
    }
}
