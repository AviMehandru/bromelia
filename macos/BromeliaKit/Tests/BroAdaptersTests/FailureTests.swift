import BroAdapters
import BroDomain
import BroFoundation
import BroPorts
import BroTestSupport
import Foundation
import Testing

/// Destinations that can't be written or go away (review follow-ups, failure testing; Linux's
/// /failure/read-only-destination and /share-gone/data-imager, Windows' FailureTests).
@Suite(.serialized) struct FailureTests {
    private struct NoSink: RunSink { func event(_ event: RobotEvent) {} }

    /// A folder that can't be written (0555): the data disc copy and writeAtomically fail with fs.failed and leave
    /// nothing.
    @Test func aReadOnlyDestinationFailsAndLeavesNothing() async throws {
        let dir = Fixtures.temporaryDirectory + "bromelia-read-only-" + UUID().uuidString
        try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        chmod(dir, 0o555)
        defer { chmod(dir, 0o755); try? FileManager.default.removeItem(atPath: dir) }
        let drives = FakeDriveControl()
        drives.disc = FakeDisc()
        drives.disc!.sectors = 512
        do throws(BroError) {
            _ = try await DataImager(drives: drives, fs: PlatformFileSystem()).copy("/dev/rdisk9", destIso: dir + "/disc.iso", sink: NoSink(),
                                                                                    cancel: CancellationSource().token)
            Issue.record("the copy succeeded")
        } catch {
            #expect(error.code == "fs.failed")
        }
        #expect(throws: BroError.self) { try PlatformFileSystem().writeAtomically(dir + "/r.json", bytes: Array("{}".utf8), mode: 0o644) }
        #expect(try FileManager.default.contentsOfDirectory(atPath: dir) == [])
    }

    /// A share that goes away during a copy (opt-in: BROMELIA_TEST_UNMOUNT_DIR, a folder on a mounted share, and
    /// BROMELIA_TEST_UNMOUNT_CMD, the shell command that unmounts it or stops its server, run 2 s into the copy): the
    /// copy fails with fs.failed or fs.notFound, never succeeds; how long it took is printed.
    @Test func aShareThatGoesAwayFailsTheCopy() async throws {
        let env = ProcessInfo.processInfo.environment
        guard let dir = env["BROMELIA_TEST_UNMOUNT_DIR"], let command = env["BROMELIA_TEST_UNMOUNT_CMD"], !dir.isEmpty, !command.isEmpty else { return }
        let drives = FakeDriveControl()
        drives.disc = FakeDisc()
        drives.disc!.sectors = 200 * 512
        drives.disc!.afterRead = { _ in usleep(50_000) }
        let later = Thread {
            sleep(2)
            let p = Process()
            p.executableURL = URL(fileURLWithPath: "/bin/sh")
            p.arguments = ["-c", command]
            try? p.run()
            p.waitUntilExit()
        }
        later.start()
        let start = Date()
        do throws(BroError) {
            _ = try await DataImager(drives: drives, fs: PlatformFileSystem()).copy("/dev/rdisk9", destIso: dir + "/disc.iso", sink: NoSink(),
                                                                                    cancel: CancellationSource().token)
            Issue.record("the copy succeeded")
        } catch {
            print("share gone: \(error.code) after \(String(format: "%.1f", Date().timeIntervalSince(start))) s")
            #expect(error.code == "fs.failed" || error.code == "fs.notFound")
        }
    }
}
