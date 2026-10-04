import BroDomain
import BroFoundation

/// Optical drives as the operating system sees them.
public protocol DeviceMonitor: Sendable {
    /// Reports events to sink (on the monitor's thread) until stop.
    func start(_ sink: @escaping @Sendable (DeviceEvent) -> Void)

    func stop()

    func currentDrives() -> [OsDrive]
}
