import BroTestSupport
import Foundation
import Testing

/// What a crash leaves behind (review follow-ups, crash points; the same as Linux's /crash/* and Windows' CrashTests):
/// BroTestProbe kills itself (SIGKILL) at the chosen point, the test looks at what is left. Phase 3's RecoveryService
/// builds on these.
@Suite(.serialized) struct CrashTests {
    /// BroTestProbe, built next to this test bundle (the test target depends on it).
    static var probe: String {
        Bundle(for: ProbeLocator.self).bundleURL.deletingLastPathComponent().appendingPathComponent("BroTestProbe").path
    }

    func scratch() throws -> String {
        let dir = Fixtures.temporaryDirectory + "bromelia-crash-" + UUID().uuidString
        try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        return dir
    }

    /// Runs the probe to its end; its termination (SIGKILL for a crash).
    func run(_ mode: String, _ folder: String) throws -> (Process.TerminationReason, Int32) {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: Self.probe)
        p.arguments = [mode, folder]
        try p.run()
        p.waitUntilExit()
        return (p.terminationReason, p.terminationStatus)
    }

    /// A data disc copy killed after two of its four chunks: the hidden .part file is left, never disc.iso.
    @Test func aDataDiscCopyKilledHalfwayLeavesOnlyThePartFile() throws {
        let dir = try scratch()
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let (reason, status) = try run("image", dir)
        #expect(reason == .uncaughtSignal && status == SIGKILL)
        let left = try FileManager.default.contentsOfDirectory(atPath: dir)
        #expect(left.count == 1)
        #expect(left.first?.hasPrefix(".disc.iso.part-") == true)
    }

    /// moveMerging killed around its third report: whatever it reported has moved, and killed before the report,
    /// exactly one more item moved without one.
    @Test(arguments: [("move-before", 2), ("move-after", 3)])
    func moveMergingKilledReportsOnlyWhatMoved(mode: String, reported: Int) throws {
        let dir = try scratch()
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let fm = FileManager.default
        try fm.createDirectory(atPath: dir + "/from", withIntermediateDirectories: true)
        try fm.createDirectory(atPath: dir + "/to", withIntermediateDirectories: true)
        for i in 0..<8 { try "f\(i)".write(toFile: dir + "/from/f\(i).mkv", atomically: false, encoding: .utf8) }
        let (reason, status) = try run(mode, dir)
        #expect(reason == .uncaughtSignal && status == SIGKILL)
        let lines = ((try? String(contentsOfFile: dir + "/report.txt", encoding: .utf8)) ?? "").split(separator: "\n").map(String.init)
        for line in lines {
            let pair = line.split(separator: "\t").map(String.init)
            #expect(!fm.fileExists(atPath: pair[0]), "\(pair[0])")
            #expect(fm.fileExists(atPath: pair[1]), "\(pair[1])")
        }
        #expect(lines.count == reported)
        #expect((0..<8).filter { !fm.fileExists(atPath: dir + "/from/f\($0).mkv") }.count == 3)
    }
}

/// Finds this test bundle, and next to it the probe.
final class ProbeLocator {}
