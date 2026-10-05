import BroDomain
import BroFoundation
import BroPorts
import DiskArbitration
import Foundation
import IOKit

/// DriveControl on macOS (plan §10.3; shared/fixtures/adapters/drive-control.cases.json), today's DiscEjector,
/// DriveControl and DiscContentProbe. A drive is its IORegistry path (PlatformDeviceMonitor); its disc is the BSD disk
/// below it (a /dev/diskN path works too). Eject: DiskArbitration (unmount, then eject), else diskutil. Close tray:
/// drutil, by the drive's product name (macOS has no per-device tray call). Content: cddafs is an audio CD, a DVD /
/// Blu-ray structure is video, other optical media data. Raw reads: /dev/rdiskN, or a disc image file.
public final class PlatformDriveControl: DriveControl, @unchecked Sendable {
    private static let sector = 2048
    private let launcher: any ProcessLauncher
    private let clock: any Clock

    public init(launcher: any ProcessLauncher, clock: any Clock) {
        self.launcher = launcher
        self.clock = clock
    }

    /// Drive numbers from `drutil list` whose product name appears in `driveName` (all drives when it is empty).
    public static func drutilIndices(_ lines: [String], matching driveName: String) -> [Int] {
        let name = driveName.split(whereSeparator: { $0 == " " || $0 == "\t" }).joined(separator: " ").lowercased()
        var indices: [Int] = []
        for line in lines {
            let f = line.split(separator: " ", omittingEmptySubsequences: true).map(String.init)
            guard f.count >= 3, let n = Int(f[0]) else { continue }
            if name.isEmpty || name.contains(f[2].lowercased()) { indices.append(n) }
        }
        return indices
    }

    public func eject(_ device: String) async throws(BroError) {
        guard let bsd = Self.bsd(device) else { throw ejectFailed(device) }
        if await Self.daEject(bsd) { return }
        let (status, _) = await run("/usr/sbin/diskutil", ["eject", "/dev/" + bsd], stall: 60)
        if status != 0 { throw ejectFailed(device) }
    }

    public func closeTray(_ device: String) async throws(BroError) {
        let (status, lines) = await run("/usr/bin/drutil", ["list"], stall: 20)
        let indices = status == 0 ? Self.drutilIndices(lines, matching: Self.product(device)) : []
        var ok = !indices.isEmpty
        for i in indices { ok = await run("/usr/bin/drutil", ["-drive", String(i), "tray", "close"], stall: 30).0 == 0 && ok }
        if !ok { throw BroMessage(.driveCloseTrayFailed, [("drive", .string(device))], severity: .error).toError() }
    }

    /// Where the disc is mounted, checked every half second; none after the timeout, when cancelled, or for something
    /// that isn't a drive.
    public func waitForMount(_ device: String, timeout: Duration, cancel: CancellationToken) async -> String? {
        guard Self.isDrive(device) else { return nil }
        let deadline = clock.monotonic().seconds + timeout.seconds
        while !cancel.isCancelled {
            if let bsd = Self.bsd(device), let path = Self.description(bsd)?[kDADiskDescriptionVolumePathKey] as? URL { return path.path }
            if clock.monotonic().seconds >= deadline { return nil }
            do throws(BroError) { try await clock.sleep(Duration(seconds: 0.5), cancel: cancel) } catch { return nil }
        }
        return nil
    }

    public func probeContent(_ device: String) -> DiscContent {
        guard Self.isDrive(device), let bsd = Self.bsd(device), let description = Self.description(bsd) else { return .unknown }
        if let kind = description[kDADiskDescriptionVolumeKindKey] as? String, kind == "cddafs" { return .audio }
        if let path = (description[kDADiskDescriptionVolumePathKey] as? URL)?.path,
           let names = try? FileManager.default.contentsOfDirectory(atPath: path),
           names.contains(where: { ["BDMV", "VIDEO_TS", "HVDVD_TS"].contains($0.uppercased()) }) {
            return .video
        }
        if let media = description[kDADiskDescriptionMediaKindKey] as? String, ["IOCDMedia", "IODVDMedia", "IOBDMedia"].contains(media) { return .data }
        return .unknown
    }

    public func openRaw(_ device: String) throws(BroError) -> any SectorReader {
        let path: String
        if Self.isDrive(device) {
            guard let bsd = Self.bsd(device) else { throw BroMessage(.driveNoDisc, severity: .error).toError() }
            path = "/dev/r" + bsd
        } else {
            path = device
        }
        let fd = open(path, O_RDONLY | O_CLOEXEC)
        guard fd >= 0 else {
            if errno == ENOENT { throw BroMessage(.fsNotFound, [("path", .string(path))], severity: .error).toError() }
            throw Self.failed("open", path, String(cString: strerror(errno)))
        }
        var st = stat()
        let bytes: Int64
        if fstat(fd, &st) == 0, (st.st_mode & S_IFMT) == S_IFREG {
            bytes = Int64(st.st_size)
        } else {
            var count: UInt64 = 0, size: UInt32 = 0
            guard ioctl(fd, DKIOCGETBLOCKCOUNT_, &count) == 0, ioctl(fd, DKIOCGETBLOCKSIZE_, &size) == 0 else {
                let reason = String(cString: strerror(errno))
                Darwin.close(fd)
                throw BroMessage(.driveSizeUnknown, [("path", .string(path)), ("reason", .string(reason))], severity: .error).toError()
            }
            bytes = Int64(count) * Int64(size)
        }
        // A partial last sector would be left out of every image.
        guard bytes % Int64(Self.sector) == 0 else {
            Darwin.close(fd)
            throw BroMessage(.drivePartialSector, [("path", .string(path)), ("size", .integer(bytes))], severity: .error).toError()
        }
        return RawReader(fd: fd, path: path, sectors: bytes / Int64(Self.sector))
    }

    // MARK: - Helpers

    /// Whether `device` names a drive (an IORegistry path or a /dev/disk path) rather than a file.
    private static func isDrive(_ device: String) -> Bool { device.hasPrefix("IOService:") || device.hasPrefix("/dev/") }

    /// The disc's BSD name ("disk4") from an IORegistry path or a /dev/(r)diskN path.
    private static func bsd(_ device: String) -> String? {
        if device.hasPrefix("IOService:") { return PlatformDeviceMonitor.bsdName(device) }
        let name = device.replacingOccurrences(of: "/dev/r", with: "").replacingOccurrences(of: "/dev/", with: "")
        return name.isEmpty ? nil : name
    }

    /// The drive's product name, for drutil.
    private static func product(_ device: String) -> String {
        guard device.hasPrefix("IOService:") else { return "" }
        let entry = IORegistryEntryFromPath(kIOMainPortDefault, device)
        guard entry != 0 else { return "" }
        defer { IOObjectRelease(entry) }
        let traits = IORegistryEntryCreateCFProperty(entry, "Device Characteristics" as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue() as? [String: Any]
        return [traits?["Vendor Name"] as? String, traits?["Product Name"] as? String].compactMap { $0 }.joined(separator: " ")
    }

    private static func description(_ bsd: String) -> [CFString: Any]? {
        guard let session = DASessionCreate(kCFAllocatorDefault), let disk = DADiskCreateFromBSDName(kCFAllocatorDefault, session, bsd) else { return nil }
        return DADiskCopyDescription(disk) as? [CFString: Any]
    }

    /// Unmounts (MakeMKV may have left it mounted), then ejects; whether DiskArbitration did.
    private static func daEject(_ bsd: String) async -> Bool {
        guard let session = DASessionCreate(kCFAllocatorDefault), let disk = DADiskCreateFromBSDName(kCFAllocatorDefault, session, bsd) else { return false }
        DASessionSetDispatchQueue(session, DispatchQueue.global())
        defer { DASessionSetDispatchQueue(session, nil) }
        final class Box: @unchecked Sendable { var continuation: CheckedContinuation<Bool, Never>? }
        let box = Box()
        let callback: DADiskEjectCallback = { _, dissenter, context in
            guard let context else { return }
            Unmanaged<Box>.fromOpaque(context).takeRetainedValue().continuation?.resume(returning: dissenter == nil)
        }
        return await withCheckedContinuation { c in
            box.continuation = c
            DADiskUnmount(disk, DADiskUnmountOptions(kDADiskUnmountOptionForce), nil, nil)
            DADiskEject(disk, DADiskEjectOptions(kDADiskEjectOptionDefault), callback, Unmanaged.passRetained(box).toOpaque())
        }
    }

    /// A system tool's exit status and stdout; -1 when it can't start.
    private func run(_ tool: String, _ arguments: [String], stall: Double) async -> (Int, [String]) {
        guard let process = try? launcher.start(ProcessSpec(executable: tool, arguments: arguments, environment: [:], stopPolicy: .terminateFirst,
                                                            stallTimeout: Duration(seconds: stall)))
        else { return (-1, []) }
        var lines: [String] = []
        for await line in process.lines() where line.stream == .stdout { lines.append(line.text) }
        return (await process.wait().status, lines)
    }

    private func ejectFailed(_ device: String) -> BroError {
        BroMessage(.driveEjectFailed, [("device", .string(device))], severity: .error).toError()
    }

    fileprivate static func failed(_ operation: String, _ path: String, _ reason: String) -> BroError {
        BroMessage(.fsFailed, [("operation", .string(operation)), ("path", .string(path)), ("reason", .string(reason))], severity: .error).toError()
    }

    /// Whole sectors from a file or a raw disk.
    private final class RawReader: SectorReader, @unchecked Sendable {
        private let lock = NSLock()
        private var fd: Int32
        private let path: String
        private let sectors: Int64

        init(fd: Int32, path: String, sectors: Int64) {
            self.fd = fd
            self.path = path
            self.sectors = sectors
        }

        deinit { close() }

        func read(_ sector: Int64, count: Int) throws(BroError) -> [UInt8] {
            let n = Int(max(0, min(Int64(count), sectors - sector))) * PlatformDriveControl.sector
            var buffer = [UInt8](repeating: 0, count: n)
            var total = 0
            let failure: String? = lock.withLock {
                while total < n {
                    let got = buffer.withUnsafeMutableBytes { pread(fd, $0.baseAddress! + total, n - total, off_t(sector) * 2048 + off_t(total)) }
                    if got < 0 && errno == EINTR { continue }
                    if got < 0 { return String(cString: strerror(errno)) }
                    if got == 0 { break }
                    total += got
                }
                return nil
            }
            if let failure { throw PlatformDriveControl.failed("read", path, failure) }
            return Array(buffer.prefix(total))
        }

        func sectorCount() throws(BroError) -> Int64 { sectors }

        func close() {
            lock.withLock {
                if fd >= 0 { Darwin.close(fd) }
                fd = -1
            }
        }
    }
}

/// <sys/disk.h>'s DKIOCGETBLOCKCOUNT and DKIOCGETBLOCKSIZE (Swift doesn't import the macros).
private let DKIOCGETBLOCKCOUNT_: UInt = 0x4008_6419
private let DKIOCGETBLOCKSIZE_: UInt = 0x4004_6418
