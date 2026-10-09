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

    static func alive(_ pid: pid_t) -> Bool { kill(pid, 0) == 0 || errno == EPERM }

    static func pid(_ path: String) -> pid_t? {
        (try? String(contentsOfFile: path, encoding: .utf8)).flatMap { pid_t($0.trimmingCharacters(in: .whitespacesAndNewlines)) }
    }

    /// The engine killed while a tool runs: the tool and its process group end within a few seconds (Lifeline; on
    /// Windows KILL_ON_JOB_CLOSE), so a crash never leaves makemkvcon reading the disc into a folder marked INCOMPLETE.
    @Test func aToolDoesNotOutliveAKilledEngine() throws {
        let dir = try scratch()
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let (reason, status) = try run("tool-then-die", dir)
        #expect(reason == .uncaughtSignal && status == SIGKILL)
        let tool = try #require(Self.pid(dir + "/tool.pid")), child = try #require(Self.pid(dir + "/child.pid"))
        let deadline = Date().addingTimeInterval(10)
        while (Self.alive(tool) || Self.alive(child)) && Date() < deadline { usleep(100_000) }
        #expect(!Self.alive(tool), "the tool still runs")
        #expect(!Self.alive(child), "the tool's background child still runs")
        if Self.alive(child) { kill(child, SIGKILL) }
        if Self.alive(tool) { kill(tool, SIGKILL) }
    }

    /// A tool that exited normally takes its watcher with it: what it left running is left alone, even after the engine
    /// is killed.
    @Test func whatAFinishedToolLeftRunningIsLeftAlone() throws {
        let dir = try scratch()
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let (reason, status) = try run("tool-exits-then-die", dir)
        #expect(reason == .uncaughtSignal && status == SIGKILL)
        let tool = try #require(Self.pid(dir + "/tool.pid")), child = try #require(Self.pid(dir + "/child.pid"))
        defer { kill(child, SIGKILL) }
        let pgrep = Process()
        pgrep.executableURL = URL(fileURLWithPath: "/usr/bin/pgrep")
        pgrep.arguments = ["-f", "bromelia-lifeline \(tool)$"]
        pgrep.standardOutput = FileHandle.nullDevice
        try pgrep.run()
        pgrep.waitUntilExit()
        #expect(pgrep.terminationStatus == 1, "the watcher is still there")
        sleep(1)
        #expect(Self.alive(child))
    }
}

/// Finds this test bundle, and next to it the probe.
final class ProbeLocator {}
