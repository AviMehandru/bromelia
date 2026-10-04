import BroDomain
import BroFoundation
import BroPorts
import Foundation

/// A DeviceMonitor that looks at the drives every interval and reports what changed (OsDriveState.changes;
/// shared/fixtures/adapters/os-drive-states.cases.json). The platform monitors are this over the system's snapshot.
public final class DrivePoller: DeviceMonitor, @unchecked Sendable {
    private let snapshot: @Sendable () -> [OsDriveState]
    private let clock: any Clock
    private let interval: Duration
    private let lock = NSLock()
    private var last: [OsDriveState] = []
    private var timer: (any TimerHandle)?
    private var sink: (@Sendable (DeviceEvent) -> Void)?

    public init(snapshot: @escaping @Sendable () -> [OsDriveState], clock: any Clock, interval: Duration) {
        self.snapshot = snapshot
        self.clock = clock
        self.interval = interval
    }

    /// Takes a snapshot now (no events for what is already there), then one each interval.
    public func start(_ sink: @escaping @Sendable (DeviceEvent) -> Void) {
        lock.lock()
        defer { lock.unlock() }
        guard timer == nil else { return }
        self.sink = sink
        last = snapshot()
        timer = clock.timer(.every(interval: interval)) { [weak self] in self?.look() }
    }

    public func stop() {
        let t: (any TimerHandle)? = lock.withLock {
            defer { timer = nil; sink = nil }
            return timer
        }
        t?.cancel()
    }

    public func currentDrives() -> [OsDrive] { snapshot().map(\.drive) }

    /// One look; the events go to the sink outside the lock (the sink may stop the poller).
    private func look() {
        let found: (sink: @Sendable (DeviceEvent) -> Void, events: [DeviceEvent])? = lock.withLock {
            guard let sink else { return nil }
            let now = snapshot()
            defer { last = now }
            return (sink, OsDriveState.changes(last, after: now))
        }
        guard let found else { return }
        for e in found.events { found.sink(e) }
    }
}
