import Foundation
import DiskArbitration
import UserNotifications
import AppKit

/// Watches for optical media being inserted or removed using DiskArbitration, so drives can be
/// rescanned immediately instead of waiting for the next poll.
final class OpticalMediaWatcher {
    private var session: DASession?
    private let onChange: (String) -> Void

    init(onChange: @escaping (String) -> Void) {
        self.onChange = onChange
    }

    func start() {
        guard session == nil, let s = DASessionCreate(kCFAllocatorDefault) else { return }
        session = s
        let context = Unmanaged.passUnretained(self).toOpaque()
        let appeared: DADiskAppearedCallback = { disk, ctx in
            guard let ctx else { return }
            let me = Unmanaged<OpticalMediaWatcher>.fromOpaque(ctx).takeUnretainedValue()
            me.handle(disk)
        }
        let disappeared: DADiskDisappearedCallback = { disk, ctx in
            guard let ctx else { return }
            let me = Unmanaged<OpticalMediaWatcher>.fromOpaque(ctx).takeUnretainedValue()
            me.handle(disk)
        }
        DARegisterDiskAppearedCallback(s, nil, appeared, context)
        DARegisterDiskDisappearedCallback(s, nil, disappeared, context)
        DASessionSetDispatchQueue(s, DispatchQueue.main)
    }

    func stop() {
        if let s = session { DASessionSetDispatchQueue(s, nil) }
        session = nil
    }

    private func handle(_ disk: DADisk) {
        guard let desc = DADiskCopyDescription(disk) as? [CFString: Any] else { return }
        let kind = desc[kDADiskDescriptionMediaKindKey] as? String ?? ""
        guard kind == "IOCDMedia" || kind == "IODVDMedia" || kind == "IOBDMedia" else { return }
        // Only whole-media objects, not individual partitions / sessions.
        if let whole = desc[kDADiskDescriptionMediaWholeKey] as? Bool, !whole { return }
        let bsd = DADiskGetBSDName(disk).map { String(cString: $0) } ?? ""
        onChange(bsd)
    }
}

enum DiscEjector {
    /// Ejects the disc in the drive with the given device path (/dev/rdisk4 or /dev/disk4).
    static func eject(devicePath: String) async -> Bool {
        let bsd = devicePath.replacingOccurrences(of: "/dev/r", with: "").replacingOccurrences(of: "/dev/", with: "")
        guard !bsd.isEmpty else { return false }
        if await daEject(bsdName: bsd) { return true }
        // Fallback to diskutil. (drutil is not used: it cannot target a specific drive reliably.)
        return await runTool("/usr/sbin/diskutil", ["eject", "/dev/\(bsd)"])
    }

    private static func daEject(bsdName: String) async -> Bool {
        guard let session = DASessionCreate(kCFAllocatorDefault),
              let disk = DADiskCreateFromBSDName(kCFAllocatorDefault, session, bsdName) else { return false }
        DASessionSetDispatchQueue(session, DispatchQueue.global())
        defer { DASessionSetDispatchQueue(session, nil) }
        final class Box: @unchecked Sendable { var cont: CheckedContinuation<Bool, Never>? }
        let box = Box()
        let callback: DADiskEjectCallback = { _, dissenter, ctx in
            guard let ctx else { return }
            let b = Unmanaged<Box>.fromOpaque(ctx).takeRetainedValue()
            b.cont?.resume(returning: dissenter == nil)
        }
        return await withCheckedContinuation { cont in
            box.cont = cont
            // Unmount first (MakeMKV may have left it mounted), then eject.
            DADiskUnmount(disk, DADiskUnmountOptions(kDADiskUnmountOptionForce), nil, nil)
            DADiskEject(disk, DADiskEjectOptions(kDADiskEjectOptionDefault), callback, Unmanaged.passRetained(box).toOpaque())
        }
    }

    private static func runTool(_ path: String, _ args: [String]) async -> Bool {
        let runner = ProcessRunner(executable: URL(fileURLWithPath: path), arguments: args)
        let out = try? await runner.run(timeout: 60) { _ in }
        return out?.exitCode == 0
    }
}

enum Notifier {
    static func requestAuthorization() {
        UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound]) { _, _ in }
    }

    static func post(title: String, body: String, sound: Bool) {
        let content = UNMutableNotificationContent()
        content.title = title
        content.body = body
        if sound { content.sound = .default }
        let req = UNNotificationRequest(identifier: UUID().uuidString, content: content, trigger: nil)
        UNUserNotificationCenter.current().add(req) { error in
            if error != nil, sound {
                DispatchQueue.main.async { NSSound.beep() }
            }
        }
    }
}
