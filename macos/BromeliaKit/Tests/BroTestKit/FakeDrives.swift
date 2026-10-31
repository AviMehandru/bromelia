import BroDomain
import BroFoundation
import BroPorts
import Foundation

/// The operating system's drives, faked (plan §17): what `currentDrives` lists, the events `send` reports to the
/// monitor's sink, the content `probeContent` finds for a device (video unless `setContent` says otherwise), and the tray
/// calls, recorded in `calls()` ("eject <device>", "closeTray <device>"). A mount wait answers the drive's mount path at
/// once. Raw sector reads aren't faked. Thread-safe.
public final class FakeDrives: DeviceMonitor, DriveControl, @unchecked Sendable {
    private let lock = NSLock()
    private var drives: [OsDrive] = []
    private var contents: [String: DiscContent] = [:]
    private var recorded: [String] = []
    private var sink: (@Sendable (DeviceEvent) -> Void)?

    public init() {}

    /// A drive the OS lists from now on.
    public func add(_ drive: OsDrive) {
        lock.withLock {
            drives.removeAll { $0.device == drive.device }
            drives.append(drive)
        }
    }

    /// What `probeContent` finds on the device (the one the engine names: MakeMKV's).
    public func setContent(_ device: String, _ content: DiscContent) { lock.withLock { contents[device] = content } }

    /// Reports the event to the monitor's sink (nothing before start or after stop).
    public func send(_ event: DeviceEvent) {
        let sink = lock.withLock { self.sink }
        sink?(event)
    }

    /// Whether the monitor is started (a sink is kept).
    public func monitoring() -> Bool { lock.withLock { sink != nil } }

    public func calls() -> [String] { lock.withLock { recorded } }

    public func start(_ sink: @escaping @Sendable (DeviceEvent) -> Void) { lock.withLock { self.sink = sink } }

    public func stop() { lock.withLock { sink = nil } }

    public func currentDrives() -> [OsDrive] { lock.withLock { drives } }

    public func eject(_ device: String) async throws(BroError) { lock.withLock { recorded.append("eject " + device) } }

    public func closeTray(_ device: String) async throws(BroError) { lock.withLock { recorded.append("closeTray " + device) } }

    public func waitForMount(_ device: String, timeout: Duration, cancel: CancellationToken) async -> String? {
        lock.withLock { drives.first { $0.device == device }?.mountPath }
    }

    public func probeContent(_ device: String) -> DiscContent { lock.withLock { contents[device] ?? .video } }

    public func openRaw(_ device: String) throws(BroError) -> any SectorReader {
        throw BroError("internal.unexpected", [("detail", .string("raw sector reads aren't faked"))])
    }
}
