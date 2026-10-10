import BroDomain
import BroFoundation
import BroPorts

/// The Clock port as engine code sees it: now and monotonic as the clock says; a timer's handler runs on the
/// executor, whatever thread the clock fires it on, and never after the timer was cancelled.
public final class EngineClock: Sendable {
    private let clock: any Clock
    private let executor: EngineExecutor

    public init(clock: any Clock, executor: EngineExecutor) {
        self.clock = clock
        self.executor = executor
    }

    public func now() -> Instant { clock.now() }

    public func monotonic() -> Duration { clock.monotonic() }

    @EngineActor
    public func timer(_ schedule: TimerSchedule, handler: @escaping @EngineActor () -> Void) -> EngineTimer {
        var once = false
        if case .at = schedule { once = true }
        let timer = EngineTimer(handler: handler, once: once)
        let executor = executor
        timer.handle = clock.timer(schedule) {
            executor.post { timer.fire() }
        }
        return timer
    }
}
