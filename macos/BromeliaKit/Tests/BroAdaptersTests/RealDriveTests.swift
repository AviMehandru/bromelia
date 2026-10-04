import BroAdapters
import BroDomain
import BroFoundation
import BroPorts
import Foundation
import Testing

/// The platform adapters with a real optical drive holding a video DVD: only when BROMELIA_TEST_DRIVE is set (the owner
/// attaches the drive and asks). The drive list, the content probe, raw reads, VIDEO_TS on the mounted disc, MakeMKV's
/// drive list and listing, and last of all eject (and close tray, which a slim drive can't do) with the monitor watching.
/// BROMELIA_TEST_DRIVE_IMAGE also images the whole disc with DataImager and reads the image back.
@Suite(.serialized) struct RealDriveTests {
    static let enabled = !(ProcessInfo.processInfo.environment["BROMELIA_TEST_DRIVE"] ?? "").isEmpty
    static let image = !(ProcessInfo.processInfo.environment["BROMELIA_TEST_DRIVE_IMAGE"] ?? "").isEmpty

    final class Events: RunSink, @unchecked Sendable {
        private let lock = NSLock()
        private var all: [RobotEvent] = []
        var events: [RobotEvent] { lock.withLock { all } }
        func event(_ event: RobotEvent) { lock.withLock { all.append(event) } }
    }

    final class DeviceEvents: @unchecked Sendable {
        private let lock = NSLock()
        private var all: [DeviceEvent] = []
        var events: [DeviceEvent] { lock.withLock { all } }
        func add(_ e: DeviceEvent) { lock.withLock { all.append(e) } }
    }

    /// The first drive with a disc.
    func drive() throws -> OsDriveState {
        let states = PlatformDeviceMonitor(clock: SystemClock()).snapshot()
        for s in states { print("drive: \(s.drive.device) | \(s.drive.identification) | media \(s.media) | mounted at \(s.drive.mountPath ?? "-")") }
        return try #require(states.first { $0.media }, "no drive holds a disc")
    }

    func locator(_ fs: PlatformFileSystem) -> SystemToolLocator {
        SystemToolLocator(fs: fs, configured: [:], candidates: PlatformToolPaths.candidates(NSHomeDirectory()), names: PlatformToolPaths.names(),
                          searchPath: (ProcessInfo.processInfo.environment["PATH"] ?? "").split(separator: ":").map(String.init), home: NSHomeDirectory())
    }

    @Test(.enabled(if: enabled)) func theDriveAndItsDisc() async throws {
        let state = try drive()
        let device = state.drive.device
        #expect(device.hasPrefix("IOService:"))
        #expect(!state.drive.identification.isEmpty)
        let control = PlatformDriveControl(launcher: PlatformProcessLauncher(), clock: SystemClock())

        // Mounted (macOS mounts a DVD by itself), and its content.
        let mount = await control.waitForMount(device, timeout: Duration(seconds: 30), cancel: CancellationToken())
        let path = try #require(mount, "not mounted")
        #expect(state.drive.mountPath == path)
        let content = control.probeContent(device)
        print("content: \(content.rawValue), mounted at \(path)")
        #expect(content == .video)

        // Raw reads: the ISO 9660 / UDF bridge descriptors, and the whole disc's size.
        let reader = try control.openRaw(device)
        defer { reader.close() }
        let sectors = try reader.sectorCount()
        print("sectors: \(sectors) (\(sectors * 2048) bytes)")
        #expect(sectors > 1_000_000)
        let descriptors = try reader.read(16, count: 3)
        #expect(descriptors.count == 3 * 2048)
        let ids = (0..<3).map { String(decoding: descriptors[($0 * 2048 + 1)..<($0 * 2048 + 6)], as: UTF8.self) }
        print("volume descriptors: \(ids)")
        #expect(ids.contains("CD001") || ids.contains("BEA01"))

        // VIDEO_TS: the same files through the mount and through the raw disc (as an image).
        let mounted = try #require(VideoTsByteSource.open(path))
        let analysis = try #require(DvdNav.analyse(mounted))
        print("VIDEO_TS: \(mounted.files().count) files, \(analysis.titles.count) titles, \(analysis.stills.count) menu stills")
        #expect(mounted.files().contains { $0.name == "VIDEO_TS.IFO" })
        let ifo = mounted.read("VIDEO_TS.IFO", offset: 0, length: 2048)
        #expect(String(decoding: ifo.prefix(12), as: UTF8.self) == "DVDVIDEO-VMG")

        // MakeMKV sees the same drive and lists the disc.
        let fs = PlatformFileSystem()
        let locator = locator(fs)
        if locator.locate(.makemkvcon).path != nil {
            let tool = MakemkvTool(launcher: PlatformProcessLauncher(), fs: fs, isolation: HomeDirIsolation(fs: fs, layout: .macos), locator: locator)
            let drives = try await tool.scanDrives(CancellationToken())
            for d in drives { print("makemkv drive \(d.index): \(d.identification) | \(d.label) | \(d.device) | \(d.state)") }
            let product = state.drive.identification.split(separator: " ").last.map(String.init) ?? ""
            let match = try #require(drives.first { $0.identification.contains(product) && !$0.device.isEmpty }, "MakeMKV doesn't list \(product)")
            let work = URL(fileURLWithPath: NSTemporaryDirectory()).resolvingSymlinksInPath().path + "/bromelia-drive-" + UUID().uuidString
            defer { try? FileManager.default.removeItem(atPath: work) }
            let invocation = MakemkvInvocation(settings: MakemkvRunSettings(settings: [:], dataDir: "", workDirectory: work + "/home"),
                                               options: MakemkvOptions(), stallTimeout: Duration(seconds: 600), transcript: work + "/makemkv.txt")
            let listing = try await tool.listing(.drive(index: match.index, device: match.device), invocation: invocation, sink: Events(),
                                                 cancel: CancellationToken())
            print("makemkv listing: \(listing.listing.titles.count) titles, outcome \(listing.run.outcome.status)")
            #expect(listing.run.outcome.status == .success)
            #expect(listing.listing.titles.count > 0)
        }
    }

    @Test(.enabled(if: enabled && image)) func theWholeDiscBecomesAnImage() async throws {
        let state = try drive()
        let dir = URL(fileURLWithPath: NSTemporaryDirectory()).resolvingSymlinksInPath().path + "/bromelia-image-" + UUID().uuidString
        try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let control = PlatformDriveControl(launcher: PlatformProcessLauncher(), clock: SystemClock())
        let events = Events()
        let started = Date()
        let bytes = try await DataImager(drives: control, fs: PlatformFileSystem()).copy(state.drive.device, destIso: dir + "/disc.iso", sink: events,
                                                                                          cancel: CancellationToken())
        let seconds = Date().timeIntervalSince(started)
        print("imaged \(bytes) bytes in \(Int(seconds)) s (\(Int(Double(bytes) / seconds / 1_000_000)) MB/s)")
        #expect(try FileManager.default.contentsOfDirectory(atPath: dir) == ["disc.iso"])
        let iso = try #require(VideoTsByteSource.open(dir + "/disc.iso"))
        let mountPath = try #require(state.drive.mountPath)
        let mounted = try #require(VideoTsByteSource.open(mountPath))
        #expect(iso.files() == mounted.files())
        for f in mounted.files() where f.name.hasSuffix(".IFO") {
            #expect(iso.read(f.name, offset: 0, length: Int(f.size)) == mounted.read(f.name, offset: 0, length: Int(f.size)), "\(f.name) differs")
        }
        #expect(DvdNav.analyse(iso) == DvdNav.analyse(mounted))
    }

    /// Last: eject with the monitor watching; then close tray (a slim drive has no motor to close it).
    @Test(.enabled(if: enabled)) func zEjectAndCloseTray() async throws {
        let state = try drive()
        let control = PlatformDriveControl(launcher: PlatformProcessLauncher(), clock: SystemClock())
        let monitor = PlatformDeviceMonitor(clock: SystemClock(), interval: Duration(seconds: 0.5))
        let seen = DeviceEvents()
        monitor.start { seen.add($0) }
        defer { monitor.stop() }
        try await control.eject(state.drive.device)
        for _ in 0..<40 where !seen.events.contains(.mediaRemoved(device: state.drive.device)) { try await Task.sleep(nanoseconds: 250_000_000) }
        print("after eject: \(seen.events)")
        #expect(seen.events.contains(.mediaRemoved(device: state.drive.device)))
        #expect(seen.events.contains(.unmounted(device: state.drive.device)))
        do throws(BroError) {
            try await control.closeTray(state.drive.device)
            print("close tray: done")
        } catch {
            print("close tray: \(error.code) (expected on a slim drive: its tray has no motor)")
        }
    }
}
