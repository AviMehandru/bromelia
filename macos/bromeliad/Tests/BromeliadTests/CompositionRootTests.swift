import BroDomain
import BroEngine
import BroFoundation
@testable import Bromeliad
import Foundation
import Testing

/// The real root's smoke test (decision 175): a temporary data folder and home, no system keystore, no system tools (so
/// nothing runs makemkvcon), no drives, an empty environment. Never the default data folder, the system keystore or the user's home.
struct CompositionRootTests {
    @Test func theRealRootBuildsStartsAnswersAndStops() async throws {
        let root = URL(fileURLWithPath: NSTemporaryDirectory()).resolvingSymlinksInPath().path + "/bromelia-host-" + UUID().uuidString
        defer { try? FileManager.default.removeItem(atPath: root) }
        let data = root + "/data", home = root + "/home"
        try FileManager.default.createDirectory(atPath: home, withIntermediateDirectories: true)
        let engine = try await CompositionRoot.build(RootOptions(dataDir: data, configPath: nil, home: home, systemKeystore: false,
                                                                 systemTools: false, drives: false, environment: [:]))
        #expect(FileManager.default.fileExists(atPath: data + "/bromelia.db"), "the store is opened in the data folder")
        try await engine.start()
        let status = try await engine.system()
        #expect(status.facts.version == "1.0.0")
        #expect(status.facts.version == CompositionRoot.appVersion)
        #expect(status.facts.platform == "macos")
        #expect(status.facts.dataDir == data)
        #expect(status.facts.pid == Int64(ProcessInfo.processInfo.processIdentifier))
        #expect(!status.makemkvFound)
        #expect(status.tools.allSatisfy { $0.path == nil })
        #expect(status.libraries.isEmpty)
        try await engine.stop()
        #expect(await code(of: engine) == "server.stopping", "a call after stop")
        let secrets = (try? FileManager.default.contentsOfDirectory(atPath: data + "/secrets")) ?? []
        #expect(secrets.isEmpty, "no secret was written")
    }

    /// I1-5: without drives nothing scans one. With no tools the real scan fails, and start waits for the first scan, so
    /// makemkv.driveScanFailed would be active here.
    @Test func noDriveScanRunsWithoutDrives() async throws {
        let root = URL(fileURLWithPath: NSTemporaryDirectory()).resolvingSymlinksInPath().path + "/bromelia-host-" + UUID().uuidString
        defer { try? FileManager.default.removeItem(atPath: root) }
        let data = root + "/data", home = root + "/home"
        try FileManager.default.createDirectory(atPath: home, withIntermediateDirectories: true)
        let engine = try await CompositionRoot.build(RootOptions(dataDir: data, configPath: nil, home: home, systemKeystore: false,
                                                                 systemTools: false, drives: false, environment: [:]))
        try await engine.start()
        let status = try await engine.system()
        #expect(status.problems.isEmpty)
        #expect(try await engine.drives().isEmpty)
        try await engine.stop()
    }

    /// I1-5: with no drives the monitor reports nothing and every drive command is refused.
    @Test func noDrivesReportNothingAndRefuseEverything() async throws {
        let monitor = NoDeviceMonitor()
        monitor.start { _ in Issue.record("the monitor reported something") }
        #expect(monitor.currentDrives().isEmpty)
        monitor.stop()

        let control = NoDriveControl()
        await #expect(refused { () async throws(BroError) in try await control.eject("/dev/x") })
        await #expect(refused { () async throws(BroError) in try await control.closeTray("/dev/x") })
        await #expect(refused { () async throws(BroError) in _ = try control.openRaw("/dev/x") })
        #expect(await control.waitForMount("/dev/x", timeout: Duration(seconds: 1), cancel: CancellationSource().token) == nil)
        #expect(control.probeContent("/dev/x") == .unknown)
    }

    /// Whether the call fails with drive.notFound.
    private func refused(_ call: () async throws(BroError) -> Void) async -> Bool {
        do throws(BroError) {
            try await call()
            return false
        } catch {
            return error.code == "drive.notFound"
        }
    }

    /// What system() fails with (nil when it answers).
    private func code(of engine: LocalEngine) async -> String? {
        do throws(BroError) {
            _ = try await engine.system()
            return nil
        } catch {
            return error.code
        }
    }
}
