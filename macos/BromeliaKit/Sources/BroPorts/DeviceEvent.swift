import BroDomain
import BroFoundation

/// What the operating system reported about a drive.
public enum DeviceEvent: Sendable, Equatable {
    case driveAppeared(drive: OsDrive)
    case driveVanished(device: String)
    case mediaArrived(device: String)
    case mediaRemoved(device: String)
    case trayOpened(device: String)
    case mounted(device: String, path: String)
    case unmounted(device: String)
}
