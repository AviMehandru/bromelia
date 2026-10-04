import BroDomain
import BroPorts

/// An optical drive as the operating system sees it at one moment, and whether it holds media
/// (shared/fixtures/adapters/os-drive-states.cases.json).
public struct OsDriveState: Sendable, Equatable {
    public var drive: OsDrive
    public var media: Bool

    public init(drive: OsDrive, media: Bool) {
        self.drive = drive
        self.media = media
    }

    /// The events between two snapshots: for each drive of `after`, appeared or what changed (unmounted, media removed,
    /// media arrived, mounted); then each drive that is gone (unmounted, media removed, vanished).
    public static func changes(_ before: [OsDriveState], after: [OsDriveState]) -> [DeviceEvent] {
        var events: [DeviceEvent] = []
        for now in after {
            let device = now.drive.device
            let was = before.first { $0.drive.device == device }
            if let was {
                if let old = was.drive.mountPath, old != now.drive.mountPath { events.append(.unmounted(device: device)) }
                if was.media && !now.media { events.append(.mediaRemoved(device: device)) }
            } else {
                events.append(.driveAppeared(drive: now.drive))
            }
            if now.media && was?.media != true { events.append(.mediaArrived(device: device)) }
            if let path = now.drive.mountPath, path != was?.drive.mountPath { events.append(.mounted(device: device, path: path)) }
        }
        for gone in before where !after.contains(where: { $0.drive.device == gone.drive.device }) {
            if gone.drive.mountPath != nil { events.append(.unmounted(device: gone.drive.device)) }
            if gone.media { events.append(.mediaRemoved(device: gone.drive.device)) }
            events.append(.driveVanished(device: gone.drive.device))
        }
        return events
    }
}
