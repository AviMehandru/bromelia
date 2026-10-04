import BroDomain
import BroFoundation
import BroPorts
import DiskArbitration
import Foundation
import IOKit

/// DeviceMonitor on macOS (plan §10.3): the optical drives in the IORegistry (IOCDBlockStorageDevice: CD, DVD and
/// Blu-ray subclasses), polled. A drive's device is its IORegistry path (stable whether or not it holds a disc); its
/// identification is the drive's "Vendor Name Product Name"; media when it has a BSD disk below it, mounted where
/// DiskArbitration says. DrivePoller turns changes into events.
public final class PlatformDeviceMonitor: DeviceMonitor, @unchecked Sendable {
    private let poller: DrivePoller

    /// - Parameter interval: how often to look (2 s when nil).
    public init(clock: any Clock, interval: Duration? = nil) {
        poller = DrivePoller(snapshot: { PlatformDeviceMonitor.snapshotNow() }, clock: clock, interval: interval ?? Duration(seconds: 2))
    }

    public func start(_ sink: @escaping @Sendable (DeviceEvent) -> Void) { poller.start(sink) }

    public func stop() { poller.stop() }

    public func currentDrives() -> [OsDrive] { poller.currentDrives() }

    /// The optical drives now.
    public func snapshot() -> [OsDriveState] { Self.snapshotNow() }

    /// The BSD name ("disk4") of the disc in the drive at `device` (an IORegistry path), if it holds one.
    static func bsdName(_ device: String) -> String? {
        let entry = IORegistryEntryFromPath(kIOMainPortDefault, device)
        guard entry != 0 else { return nil }
        defer { IOObjectRelease(entry) }
        return IORegistryEntrySearchCFProperty(entry, kIOServicePlane, "BSD Name" as CFString, kCFAllocatorDefault,
                                               IOOptionBits(kIORegistryIterateRecursively)) as? String
    }

    private static func snapshotNow() -> [OsDriveState] {
        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOCDBlockStorageDevice"), &iterator) == KERN_SUCCESS else { return [] }
        defer { IOObjectRelease(iterator) }
        var states: [OsDriveState] = []
        while case let service = IOIteratorNext(iterator), service != 0 {
            defer { IOObjectRelease(service) }
            var path = [CChar](repeating: 0, count: 512)
            guard IORegistryEntryGetPath(service, kIOServicePlane, &path) == KERN_SUCCESS else { continue }
            let device = String(decoding: path.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }, as: UTF8.self)
            let traits = IORegistryEntryCreateCFProperty(service, "Device Characteristics" as CFString, kCFAllocatorDefault, 0)?
                .takeRetainedValue() as? [String: Any]
            let identification = [traits?["Vendor Name"] as? String, traits?["Product Name"] as? String]
                .compactMap { $0?.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }.joined(separator: " ")
            let bsd = IORegistryEntrySearchCFProperty(service, kIOServicePlane, "BSD Name" as CFString, kCFAllocatorDefault,
                                                      IOOptionBits(kIORegistryIterateRecursively)) as? String
            states.append(OsDriveState(drive: OsDrive(device: device, identification: identification, mountPath: bsd.flatMap(mountPath)), media: bsd != nil))
        }
        return states.sorted { $0.drive.device < $1.drive.device }
    }

    /// Where DiskArbitration says the disc is mounted.
    private static func mountPath(_ bsd: String) -> String? {
        guard let session = DASessionCreate(kCFAllocatorDefault), let disk = DADiskCreateFromBSDName(kCFAllocatorDefault, session, bsd),
              let description = DADiskCopyDescription(disk) as? [CFString: Any]
        else { return nil }
        return (description[kDADiskDescriptionVolumePathKey] as? URL)?.path
    }
}
