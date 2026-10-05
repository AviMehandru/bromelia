import BroAdapters
import BroDomain
import BroFoundation
import BroPorts
import Foundation
import Testing

/// shared/fixtures/adapters/platform-adapters.contract.json, the process cases, on the real macOS.
@Suite(.serialized) struct PlatformProcessLauncherTests {
    let launcher = PlatformProcessLauncher()
    let dir: String

    init() throws {
        dir = NSTemporaryDirectory() + "bromelia-launcher-" + UUID().uuidString
        try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
    }

    func spec(_ exe: String, _ args: String..., policy: StopPolicy = .interruptFirst, stall: Double? = nil) -> ProcessSpec {
        ProcessSpec(executable: exe, arguments: args, environment: [:], stopPolicy: policy, stallTimeout: stall.map { Duration(seconds: $0) })
    }

    func run(_ p: any RunningProcess) async -> ([OutputLine], ProcessExit) {
        let reader = Task { var lines: [OutputLine] = []; for await l in p.lines() { lines.append(l) }; return lines }
        let exit = await p.wait()
        return (await reader.value, exit)
    }

    func elapsed(since start: ContinuousClock.Instant) -> Double {
        let d = ContinuousClock.now - start
        return Double(d.components.seconds) + Double(d.components.attoseconds) / 1e18
    }

    /// stall-stops
    @Test func stuckProcessIsStopped() async throws {
        let start = ContinuousClock.now
        let (lines, exit) = await run(try launcher.start(spec("/bin/sh", "-c", "echo started; sleep 60", policy: .terminateFirst, stall: 2)))
        #expect(exit.stalled != nil)
        #expect(exit.status != 0)
        #expect(!exit.cancelled)
        #expect(elapsed(since: start) < 15)
        #expect(lines.map(\.text) == ["started"])
    }

    /// busy-not-stopped
    @Test func busyProcessIsNotStopped() async throws {
        let (lines, exit) = await run(try launcher.start(spec("/bin/sh", "-c", "for i in 1 2 3 4 5 6; do echo $i; sleep 0.5; done", stall: 2)))
        #expect(exit.stalled == nil)
        #expect(exit.status == 0)
        #expect(lines.map(\.text) == ["1", "2", "3", "4", "5", "6"])
    }

    /// cancel-escalates: INT is ignored, TERM 5 s later ends it.
    @Test func cancelEscalatesFromInterrupt() async throws {
        let start = ContinuousClock.now
        let p = try launcher.start(spec("/bin/sh", "-c", "trap '' INT; echo ready; sleep 60 & wait"))
        try await Task.sleep(nanoseconds: 500_000_000)
        p.stop(.cancelled)
        let (_, exit) = await run(p)
        #expect(exit.cancelled)
        #expect(exit.signal == Int(SIGTERM))
        let took = elapsed(since: start)
        #expect(took >= 5 && took < 12)
    }

    /// terminate-first, and KILL 5 s after a TERM that is ignored too.
    @Test func terminateFirstThenKill() async throws {
        let p = try launcher.start(spec("/bin/sh", "-c", "echo ready; sleep 60", policy: .terminateFirst))
        try await Task.sleep(nanoseconds: 300_000_000)
        p.stop(.shutdown)
        let (_, exit) = await run(p)
        #expect(exit.signal == Int(SIGTERM))
        #expect(exit.cancelled)

        let start = ContinuousClock.now
        let stubborn = try launcher.start(spec("/bin/sh", "-c", "trap '' INT TERM; echo ready; while :; do sleep 0.1; done", policy: .terminateFirst))
        try await Task.sleep(nanoseconds: 300_000_000)
        stubborn.stop(.cancelled)
        let (_, killed) = await run(stubborn)
        #expect(killed.signal == Int(SIGKILL))
        #expect(killed.status == -1)
        let took = elapsed(since: start)
        #expect(took >= 5 && took < 12)
    }

    /// output-drained
    @Test func aChildHoldingThePipeDoesNotHoldUpTheEnd() async throws {
        let start = ContinuousClock.now
        let (lines, exit) = await run(try launcher.start(spec("/bin/sh", "-c", "sleep 20 & echo done")))
        #expect(exit.status == 0)
        #expect(lines.map(\.text) == ["done"])
        #expect(elapsed(since: start) < 8)
    }

    /// late-output-goes-nowhere
    @Test func lateOutputGoesNowhere() async throws {
        var s = spec("/bin/sh", "-c", "(sleep 6; echo late) & echo early")
        s.transcript = dir + "/late.txt"
        let (lines, _) = await run(try launcher.start(s))
        let opened = (0..<20).map { open(dir + "/opened-\($0)", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0o644) }
        try await Task.sleep(nanoseconds: 2_000_000_000)
        for (i, fd) in opened.enumerated() {
            close(fd)
            #expect(try String(contentsOfFile: dir + "/opened-\(i)", encoding: .utf8) == "")
        }
        #expect(lines.map(\.text) == ["early"])
        let text = try String(contentsOfFile: dir + "/late.txt", encoding: .utf8)
        #expect(text.contains("\nearly\n") && !text.contains("\nlate\n"))
    }

    /// long-line-is-cut
    @Test func aLongLineIsCut() async throws {
        let (lines, exit) = await run(try launcher.start(spec("/bin/sh", "-c", "head -c 200000 /dev/zero | tr '\\0' a; echo; echo next")))
        #expect(exit.status == 0)
        #expect(lines.map(\.text) == [String(repeating: "a", count: 65536) + " [cut]", "next"])
    }

    /// a-slow-reader-holds-the-tool
    @Test func aSlowReaderHoldsTheTool() async throws {
        let marker = dir + "/done"
        let p = try launcher.start(spec("/bin/sh", "-c", "seq 1 50000; touch '\(marker)'", stall: 2))
        try await Task.sleep(nanoseconds: 3_000_000_000)
        #expect(!FileManager.default.fileExists(atPath: marker))   // waiting to write: at most 10000 lines (and the pipe) wait
        let (lines, exit) = await run(p)
        #expect(lines.map(\.text) == (1...50000).map(String.init))
        #expect(exit.stalled == nil)
        #expect(FileManager.default.fileExists(atPath: marker))
    }

    /// a-slow-reader-gets-everything
    @Test func aSlowReaderGetsEverything() async throws {
        let p = try launcher.start(spec("/bin/sh", "-c", "seq 1 30000"))
        var lines: [String] = []
        for await line in p.lines() {
            lines.append(line.text)
            if lines.count % 100 == 0 { try await Task.sleep(nanoseconds: 25_000_000) }
        }
        _ = await p.wait()
        #expect(lines == (1...30000).map(String.init))
    }

    /// process-group
    @Test func stopEndsTheWholeGroup() async throws {
        let pidFile = dir + "/child.pid"
        let p = try launcher.start(spec("/bin/sh", "-c", "sleep 60 & echo $! > '\(pidFile)'; sleep 60"))
        for _ in 0..<100 where (try? String(contentsOfFile: pidFile, encoding: .utf8))?.isEmpty ?? true {
            try await Task.sleep(nanoseconds: 50_000_000)
        }
        let child = try #require(pid_t((try String(contentsOfFile: pidFile, encoding: .utf8)).trimmingCharacters(in: .whitespacesAndNewlines)))
        p.stop(.cancelled)
        _ = await run(p)
        try await Task.sleep(nanoseconds: 300_000_000)
        #expect(kill(child, 0) != 0)
    }

    /// transcript
    @Test func theTranscriptHasEveryLineBetweenTheCommandAndTheExit() async throws {
        let path = dir + "/logs/t.txt"
        var s = spec("/bin/echo", "hi")
        s.transcript = path
        _ = await run(try launcher.start(s))
        let text = try String(contentsOfFile: path, encoding: .utf8)
            .replacingOccurrences(of: #"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z"#, with: "<instant>", options: .regularExpression)
        #expect(text == "==== <instant> $ /bin/echo hi\nhi\n==== <instant> exit status 0\n")
    }

    @Test func outputLinesSayWhichStreamTheyCameFrom() async throws {
        var s = spec("/bin/sh", "-c", "echo $BRO_TEST_VALUE; pwd; echo oops >&2")
        s.environment = ["BRO_TEST_VALUE": "from the spec"]
        s.workingDirectory = dir
        let (lines, exit) = await run(try launcher.start(s))
        #expect(exit.status == 0)
        #expect(lines.contains { $0.text == "from the spec" && $0.stream == .stdout })
        #expect(lines.contains { $0.text.hasSuffix(URL(fileURLWithPath: dir).lastPathComponent) && $0.stream == .stdout })
        #expect(lines.contains { $0.text == "oops" && $0.stream == .stderr })
    }

    @Test func aProgramThatCannotStartFailsWithCouldNotStart() throws {
        #expect(throws: BroError.self) { try launcher.start(spec(dir + "/missing")) }
        do {
            _ = try launcher.start(spec(dir + "/missing"))
        } catch {
            #expect(error.code == "process.couldNotStart")
        }
        var s = spec("/bin/echo")
        s.workingDirectory = dir + "/nope"
        do {
            _ = try launcher.start(s)
            Issue.record("started in a missing folder")
        } catch {
            #expect(error.code == "process.couldNotStart")
        }
    }

    /// interpreter
    @Test func scriptsWithoutTheExecuteBitRunThroughTheShell() async throws {
        let script = dir + "/hello.sh"
        try "echo hello $1\n".write(toFile: script, atomically: true, encoding: .utf8)
        #expect(PlatformProcessLauncher.invocation(spec(script, "x")) == CommandLine(executable: "/bin/sh", arguments: [script, "x"]))
        var custom = spec(script, "x")
        custom.interpreter = "/usr/bin/env"
        #expect(PlatformProcessLauncher.invocation(custom) == CommandLine(executable: "/usr/bin/env", arguments: [script, "x"]))
        #expect(PlatformProcessLauncher.invocation(spec("/bin/echo", "x")) == CommandLine(executable: "/bin/echo", arguments: ["x"]))
        let (lines, exit) = await run(try launcher.start(spec(script, "there")))
        #expect(exit.status == 0)
        #expect(lines.map(\.text) == ["hello there"])
    }
}
