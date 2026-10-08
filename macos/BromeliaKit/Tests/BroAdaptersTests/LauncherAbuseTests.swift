import BroAdapters
import BroDomain
import BroFoundation
import BroPorts
import BroTestSupport
import Foundation
import Testing

/// The launcher against misbehaving tools (review follow-ups, failure testing; Linux's /abuse/*, Windows'
/// LauncherAbuseTests). Opt-in, since they take a while: BROMELIA_TEST_ABUSE=1.
@Suite(.serialized) struct LauncherAbuseTests {
    let enabled = ProcessInfo.processInfo.environment["BROMELIA_TEST_ABUSE"] == "1"

    /// This process's peak resident memory, in MB.
    func peakMB() -> Double {
        var u = rusage()
        getrusage(RUSAGE_SELF, &u)
        return Double(u.ru_maxrss) / 1_048_576
    }

    /// 300 MB of random bytes, read as they come: every line arrives cut to its limit, memory stays small.
    @Test func aBinaryFloodHoldsLittleMemory() async throws {
        guard enabled else { return }
        let before = peakMB()
        let p = try PlatformProcessLauncher().start(ProcessSpec(executable: "/bin/sh", arguments: ["-c", "head -c 300000000 /dev/urandom"],
                                                                environment: [:], stopPolicy: .terminateFirst))
        var lines = 0, longest = 0
        for await line in p.lines() { lines += 1; longest = max(longest, line.text.utf8.count) }
        let exit = await p.wait()
        print("binary flood: \(lines) lines, longest \(longest) bytes, peak memory \(Int(before)) MB -> \(Int(peakMB())) MB")
        #expect(exit.status == 0)
        #expect(longest <= 65536 * 3 + 16)
        #expect(peakMB() - before < 200)
    }

    /// Five generations of shells that all ignore INT and TERM: a stop ends every one of them (KILL after the grace
    /// period).
    @Test func aDeepTreeIgnoringSignalsIsStopped() async throws {
        guard enabled else { return }
        let script = Fixtures.temporaryDirectory + "bromelia-deep-" + UUID().uuidString + ".sh"
        try "trap '' INT TERM\nif [ \"$1\" -gt 0 ]; then /bin/sh \"$0\" $(($1 - 1)) & fi\necho \"$$\"\nsleep 120\n".write(toFile: script, atomically: true, encoding: .utf8)
        defer { unlink(script) }
        let p = try PlatformProcessLauncher().start(ProcessSpec(executable: "/bin/sh", arguments: [script, "4"], environment: [:],
                                                                stopPolicy: .interruptFirst))
        var lines = p.lines().makeAsyncIterator()
        var pids: [pid_t] = []
        while pids.count < 5, let line = await lines.next() { pids.append(pid_t(line.text) ?? 0) }
        #expect(pids.count == 5)
        let start = Date()
        p.stop(.cancelled)
        while await lines.next() != nil {}
        let exit = await p.wait()
        let took = Date().timeIntervalSince(start)
        print("deep tree ignoring INT and TERM: ended after \(String(format: "%.1f", took)) s")
        #expect(exit.cancelled)
        #expect(took < 12) // interrupt first: INT, 5 s, TERM, 5 s, KILL
        usleep(500_000)
        for pid in pids { #expect(kill(pid, 0) != 0, "pid \(pid) is still running") }
    }

    /// 65000-byte lines while the reader has stopped: the queue holds no more than its byte limit, the tool waits, and
    /// a stop still ends it.
    @Test func longLinesForAReaderThatStoppedHoldLittleMemory() async throws {
        guard enabled else { return }
        var spec = ProcessSpec(executable: "/bin/sh", arguments: ["-c", "exec yes \"$(head -c 65000 /dev/zero | tr '\\0' x)\""],
                               environment: [:], stopPolicy: .terminateFirst)
        spec.transcript = nil
        let before = peakMB()
        let p = try PlatformProcessLauncher().start(spec)
        var lines = p.lines().makeAsyncIterator()
        _ = await lines.next()
        try await Task.sleep(nanoseconds: 8_000_000_000) // not reading
        let held = peakMB()
        p.stop(.cancelled)
        var drained = 0
        while await lines.next() != nil { drained += 1 }
        let exit = await p.wait()
        print("long lines, reader stopped: peak memory \(Int(before)) MB -> \(Int(held)) MB while not reading; \(drained) lines drained")
        #expect(exit.cancelled)
        #expect(held - before < 100)
    }
}
