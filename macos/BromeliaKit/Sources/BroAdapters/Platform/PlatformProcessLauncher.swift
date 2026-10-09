import BroDomain
import BroFoundation
import BroPorts
import Darwin
import Foundation

/// Starts processes with posix_spawn in their own process group and watches them (plan §10.3): stalls, stop
/// escalation (INT →) TERM → KILL 5 s apart, abandonment 30 s after KILL, output read for 5 s after exit (longer only
/// while lines wait for a reader that is still taking them), a transcript of every line. Lines are cut at 64 KiB
/// (LineSplitter) and at most `queuedLines`, or `queuedBytes` of text, wait to be read: then the reading waits too, so
/// the tool waits to write (nothing is lost) and that wait doesn't count as silence. Foundation's Process isn't used: it can't start a process
/// group.
///
/// Signals go to the whole group. Once a stopped process has ended, whatever is left of its group is killed (a
/// background child of a shell ignores SIGINT). A process left running after a normal exit is left alone. If the
/// engine ends while a process runs (a crash, kill -9), the process's group is stopped too (`Lifeline`).
public final class PlatformProcessLauncher: ProcessLauncher {
    /// How many lines, and how much of their text, may wait to be read before the reading waits: long lines for a
    /// reader that has stopped can't hold more than about 16 MiB.
    public static let queuedLines = 10000
    public static let queuedBytes = 16 * 1024 * 1024

    public init() {}

    public func start(_ spec: ProcessSpec) throws(BroError) -> any RunningProcess {
        try SpawnedProcess.start(spec)
    }

    /// The program and arguments actually run: the spec's interpreter first, else /bin/sh for a file without the
    /// execute bit (a script saved without chmod +x).
    public static func invocation(_ spec: ProcessSpec) -> CommandLine {
        if let interpreter = spec.interpreter?.trimmingCharacters(in: .whitespaces), !interpreter.isEmpty {
            return CommandLine(executable: interpreter, arguments: [spec.executable] + spec.arguments)
        }
        var isDirectory: ObjCBool = false
        if FileManager.default.fileExists(atPath: spec.executable, isDirectory: &isDirectory), !isDirectory.boolValue,
           access(spec.executable, X_OK) != 0 {
            return CommandLine(executable: "/bin/sh", arguments: [spec.executable] + spec.arguments)
        }
        return CommandLine(executable: spec.executable, arguments: spec.arguments)
    }
}

/// So that a crashed engine leaves no tools behind (as KILL_ON_JOB_CLOSE does on Windows), each process gets a
/// watcher: /bin/sh in its own process group, reading a pipe that nothing writes to. The engine holds the pipe's write
/// end (close-on-exec, never closed); when the engine ends, however it ends, the kernel closes it, the watcher's read
/// returns, and it stops the process's group: TERM, then KILL 5 s later. When the process ends first, its watcher is
/// killed and reaped before the process is reaped, so it can never signal a reused process group.
enum Lifeline {
    static let script = #"read -r _; kill -s TERM -- "-$1" 2>/dev/null; sleep 5; kill -s KILL -- "-$1" 2>/dev/null"#

    /// The pipe's read end; -1 when it couldn't be made.
    static let readEnd: Int32 = {
        var fds: [Int32] = [-1, -1]
        guard pipe(&fds) == 0 else { return -1 }
        _ = fcntl(fds[0], F_SETFD, FD_CLOEXEC)
        _ = fcntl(fds[1], F_SETFD, FD_CLOEXEC)
        return fds[0]   // fds[1] stays open until the engine ends
    }()

    /// Starts the watcher of process group `group`; nil when it couldn't start (a crash then leaves the process running).
    static func watch(_ group: pid_t) -> pid_t? {
        guard readEnd >= 0 else { return nil }
        var actions: posix_spawn_file_actions_t?
        posix_spawn_file_actions_init(&actions)
        defer { posix_spawn_file_actions_destroy(&actions) }
        posix_spawn_file_actions_adddup2(&actions, readEnd, 0)
        posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0)
        posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0)
        var attr: posix_spawnattr_t?
        posix_spawnattr_init(&attr)
        defer { posix_spawnattr_destroy(&attr) }
        posix_spawnattr_setpgroup(&attr, 0)   // out of the engine's group: a terminal's ^C doesn't reach it
        var defaults = sigset_t(), empty = sigset_t()
        sigfillset(&defaults)
        sigemptyset(&empty)
        posix_spawnattr_setsigdefault(&attr, &defaults)
        posix_spawnattr_setsigmask(&attr, &empty)
        posix_spawnattr_setflags(&attr, Int16(POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_CLOEXEC_DEFAULT))
        let words: [String] = ["/bin/sh", "-c", script, "bromelia-lifeline", String(group)], environment: [String] = ["PATH=/usr/bin:/bin"]
        let argv = words.map { strdup($0) } + [nil]
        let envp = environment.map { strdup($0) } + [nil]
        defer {
            argv.forEach { free($0) }
            envp.forEach { free($0) }
        }
        var pid: pid_t = 0
        return posix_spawn(&pid, "/bin/sh", &actions, &attr, argv, envp) == 0 ? pid : nil
    }

    /// Kills and reaps a watcher whose process has ended.
    static func end(_ watcher: pid_t) {
        kill(watcher, SIGKILL)
        var status: Int32 = 0
        while waitpid(watcher, &status, 0) == -1 && errno == EINTR {}
    }
}

/// One started process: two reader threads (stdout, stderr), a thread blocked in waitpid and a watchdog thread that
/// ticks every 250 ms. All state is behind `cond`.
private final class SpawnedProcess: RunningProcess, @unchecked Sendable {
    private let pid: pid_t
    private var watcher: pid_t?   // set before the threads start
    private let spec: ProcessSpec
    private let transcript: Transcript?
    private let cond = NSCondition()
    // Guarded by cond:
    private var queue: [OutputLine] = []
    private var queueHead = 0
    private var queuedBytes = 0      // the text of the lines not taken yet
    private var takenBytes = 0       // the text of the taken lines still in `queue`
    private var lineWaiters: [CheckedContinuation<OutputLine?, Never>] = []
    private var ended = false        // no more lines: the reader gets what is queued, then nil
    private var waitingForRoom = 0   // readers waiting for the queue: the tool isn't silent, nobody is reading
    private var lastTaken: Double    // when the reader last took a line
    private var lastOutput: Double
    private var readersLeft = 2
    private var stopReading = false
    private var waitStatus: Int32?
    private var exitedAt: Double?
    private var reason: StopReason?
    private var stalled: Duration?
    private var stopAt: Double?
    private var signalsSent = 0
    private var killedAt: Double?
    private var result: ProcessExit?
    private var waiters: [CheckedContinuation<ProcessExit, Never>] = []

    private init(pid: pid_t, spec: ProcessSpec, transcript: Transcript?) {
        self.pid = pid
        self.spec = spec
        self.transcript = transcript
        lastOutput = Self.clock()
        lastTaken = lastOutput
    }

    static func clock() -> Double { Double(clock_gettime_nsec_np(CLOCK_MONOTONIC)) / 1e9 }

    static func now() -> Instant { Instant(unixMilliseconds: Int64((Date().timeIntervalSince1970 * 1000).rounded(.down))) }

    static func start(_ spec: ProcessSpec) throws(BroError) -> SpawnedProcess {
        let cmd = PlatformProcessLauncher.invocation(spec)
        let transcript = spec.transcript.flatMap { $0.isEmpty ? nil : Transcript($0) }
        transcript?.write("==== \(Instant.format(now())) $ \(([cmd.executable] + cmd.arguments).map(ArgumentSplitter.quote).joined(separator: " "))")
        func fail(_ reason: String) -> BroError {
            transcript?.write("==== \(Instant.format(now())) could not start: \(reason)")
            transcript?.close()
            return BroMessage(.processCouldNotStart, [("tool", .string((cmd.executable as NSString).lastPathComponent)),
                                                      ("reason", .string(reason))], severity: .error).toError()
        }
        if let wd = spec.workingDirectory, !wd.isEmpty {
            var isDirectory: ObjCBool = false
            if !FileManager.default.fileExists(atPath: wd, isDirectory: &isDirectory) || !isDirectory.boolValue {
                throw fail("the working folder \(wd) doesn't exist")
            }
        }

        var out: [Int32] = [-1, -1], err: [Int32] = [-1, -1]
        guard pipe(&out) == 0 else { throw fail(String(cString: strerror(errno))) }
        guard pipe(&err) == 0 else {
            close(out[0]); close(out[1])
            throw fail(String(cString: strerror(errno)))
        }
        var actions: posix_spawn_file_actions_t?
        posix_spawn_file_actions_init(&actions)
        defer { posix_spawn_file_actions_destroy(&actions) }
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0)
        posix_spawn_file_actions_adddup2(&actions, out[1], 1)
        posix_spawn_file_actions_adddup2(&actions, err[1], 2)
        if let wd = spec.workingDirectory, !wd.isEmpty { posix_spawn_file_actions_addchdir_np(&actions, wd) }

        var attr: posix_spawnattr_t?
        posix_spawnattr_init(&attr)
        defer { posix_spawnattr_destroy(&attr) }
        // Its own process group; default signal handling and an empty mask whatever the engine has; and only the
        // descriptors named above, so a child never holds another child's pipe open.
        posix_spawnattr_setpgroup(&attr, 0)
        var defaults = sigset_t(), empty = sigset_t()
        sigfillset(&defaults)
        sigemptyset(&empty)
        posix_spawnattr_setsigdefault(&attr, &defaults)
        posix_spawnattr_setsigmask(&attr, &empty)
        posix_spawnattr_setflags(&attr, Int16(POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_CLOEXEC_DEFAULT))

        var environment = ProcessInfo.processInfo.environment
        for (k, v) in spec.environment { environment[k] = v }
        let argv = ([cmd.executable] + cmd.arguments).map { strdup($0) } + [nil]
        let envp = environment.map { strdup("\($0.key)=\($0.value)") } + [nil]
        defer {
            argv.forEach { free($0) }
            envp.forEach { free($0) }
        }
        var pid: pid_t = 0
        let rc = cmd.executable.contains("/")
            ? posix_spawn(&pid, cmd.executable, &actions, &attr, argv, envp)
            : posix_spawnp(&pid, cmd.executable, &actions, &attr, argv, envp)
        close(out[1])
        close(err[1])
        guard rc == 0 else {
            close(out[0]); close(err[0])
            throw fail(String(cString: strerror(rc)))
        }
        let process = SpawnedProcess(pid: pid, spec: spec, transcript: transcript)
        process.watcher = Lifeline.watch(pid)
        process.begin(stdout: out[0], stderr: err[0])
        return process
    }

    private func begin(stdout: Int32, stderr: Int32) {
        for (fd, source) in [(stdout, OutputSource.stdout), (stderr, OutputSource.stderr)] {
            Thread.detachNewThread { [self] in read(fd, source) }
        }
        // Waits without reaping, so the pid (and process group) can't be reused while the watchdog may still signal
        // it; marks it ended; then reaps it. A process that never ends keeps this thread.
        Thread.detachNewThread { [self] in
            var info = siginfo_t()
            while waitid(P_PID, id_t(pid), &info, WEXITED | WNOWAIT) == -1 && errno == EINTR {}
            if let w = watcher { Lifeline.end(w) }   // before the pid can be reused
            cond.lock()
            if reason != nil { kill(-pid, SIGKILL) }   // what is left of a stopped process's group
            var status: Int32 = 0
            while waitpid(pid, &status, 0) == -1 && errno == EINTR {}
            waitStatus = status
            exitedAt = Self.clock()
            cond.broadcast()
            cond.unlock()
        }
        Thread.detachNewThread { [self] in watch() }
    }

    func lines() -> AsyncStream<OutputLine> { AsyncStream(unfolding: { [self] in await nextLine() }) }

    func transcriptProblem() -> BroMessage? { transcript?.problem }

    /// The next queued line; waits for one; nil once the process has ended and the queue is empty.
    private func nextLine() async -> OutputLine? {
        await withCheckedContinuation { (c: CheckedContinuation<OutputLine?, Never>) in
            cond.lock()
            if queueHead < queue.count {
                let line = queue[queueHead]
                queueHead += 1
                queuedBytes -= line.text.utf8.count
                takenBytes += line.text.utf8.count
                if (queueHead >= 4096 && queueHead * 2 >= queue.count) || takenBytes >= PlatformProcessLauncher.queuedBytes {
                    queue.removeFirst(queueHead)
                    queueHead = 0
                    takenBytes = 0
                }
                lastTaken = Self.clock()
                cond.broadcast()   // room for a waiting reader
                cond.unlock()
                c.resume(returning: line)
            } else if ended {
                cond.unlock()
                c.resume(returning: nil)
            } else {
                lineWaiters.append(c)
                cond.unlock()
            }
        }
    }

    /// Queues a line (into the transcript first); when the queue is full, waits for room. A line after the end is
    /// dropped.
    private func emit(_ line: OutputLine) {
        transcript?.write(line.text)
        cond.lock()
        var waited = false
        while (queue.count - queueHead >= PlatformProcessLauncher.queuedLines || queuedBytes >= PlatformProcessLauncher.queuedBytes) && !ended {
            waitingForRoom += 1
            cond.wait()
            waitingForRoom -= 1
            waited = true
        }
        if waited { lastOutput = Self.clock() }
        if ended {
            cond.unlock()
            return
        }
        if !lineWaiters.isEmpty {
            let waiter = lineWaiters.removeFirst()
            lastTaken = Self.clock()
            cond.unlock()
            waiter.resume(returning: line)
            return
        }
        queue.append(line)
        queuedBytes += line.text.utf8.count
        cond.unlock()
    }

    func wait() async -> ProcessExit {
        await withCheckedContinuation { (c: CheckedContinuation<ProcessExit, Never>) in
            cond.lock()
            if let result {
                cond.unlock()
                c.resume(returning: result)
            } else {
                waiters.append(c)
                cond.unlock()
            }
        }
    }

    func stop(_ reason: StopReason) {
        cond.lock()
        beginStop(reason)
        cond.unlock()
    }

    /// With cond held.
    private func beginStop(_ why: StopReason) {
        guard reason == nil else { return }
        reason = why
        guard waitStatus == nil else { return }
        stopAt = Self.clock()
        escalate(stopAt!)
    }

    /// With cond held: the signals that are due, (INT →) TERM → KILL, 5 s apart.
    private func escalate(_ now: Double) {
        guard let stopAt, waitStatus == nil else { return }
        let plan = spec.stopPolicy == .interruptFirst ? [SIGINT, SIGTERM, SIGKILL] : [SIGTERM, SIGKILL]
        while signalsSent < plan.count && now - stopAt >= Double(signalsSent) * 5 {
            let signal = plan[signalsSent]
            kill(-pid, signal)
            signalsSent += 1
            if signal == SIGKILL { killedAt = now }
        }
    }

    private func read(_ fd: Int32, _ source: OutputSource) {
        var splitter = LineSplitter()
        var buffer = [UInt8](repeating: 0, count: 65536)
        var poller = pollfd(fd: fd, events: Int16(POLLIN), revents: 0)
        loop: while true {
            cond.lock()
            let quit = stopReading
            cond.unlock()
            if quit { break }
            let ready = poll(&poller, 1, 250)
            if ready == 0 || (ready < 0 && errno == EINTR) { continue }
            let n = buffer.withUnsafeMutableBytes { Darwin.read(fd, $0.baseAddress, $0.count) }
            if n < 0 && errno == EINTR { continue }
            if n <= 0 { break loop }
            cond.lock()
            lastOutput = Self.clock()
            cond.unlock()
            for text in splitter.feed(buffer[0..<n]) { emit(OutputLine(stream: source, text: text, at: Self.now())) }
        }
        if let last = splitter.finish() { emit(OutputLine(stream: source, text: last, at: Self.now())) }
        close(fd)
        cond.lock()
        readersLeft -= 1
        cond.broadcast()
        cond.unlock()
    }

    private func watch() {
        cond.lock()
        defer { cond.unlock() }
        while result == nil {
            let now = Self.clock()
            if let limit = spec.stallTimeout, reason == nil, waitStatus == nil, waitingForRoom == 0, now - lastOutput >= limit.seconds {
                stalled = Duration(seconds: ((now - lastOutput) * 1000).rounded() / 1000)
                beginStop(.stalled)
            }
            escalate(now)
            if waitStatus == nil, let killedAt, now - killedAt >= 30 {
                stopReading = true
                finish(ProcessExit(status: -1, stalled: stalled, abandoned: true, cancelled: cancelled))
                break
            }
            if let exitedAt, let status = waitStatus {
                // Output normally ends with the process; a child it left behind may keep a pipe open: stop reading 5 s
                // after the exit, unless lines are still waiting for a reader that keeps taking them. Queued lines count
                // too: each take wakes this thread as well, often just after a reader has queued a line and before it
                // waits for room again, and ending then would drop what it hasn't read yet.
                let backlog = (waitingForRoom > 0 || queueHead < queue.count) && now - lastTaken < 5
                if readersLeft == 0 || (now - exitedAt >= 5 && !backlog) {
                    stopReading = true
                    let exited = (status & 0x7f) == 0
                    finish(exited ? ProcessExit(status: Int((status >> 8) & 0xff), stalled: stalled, cancelled: cancelled)
                                  : ProcessExit(status: -1, signal: Int(status & 0x7f), stalled: stalled, cancelled: cancelled))
                    break
                }
            }
            cond.wait(until: Date(timeIntervalSinceNow: 0.25))
        }
    }

    private var cancelled: Bool { reason == .cancelled || reason == .shutdown }

    /// With cond held.
    private func finish(_ exit: ProcessExit) {
        result = exit
        ended = true
        let readers = queueHead < queue.count ? [] : lineWaiters
        if queueHead >= queue.count { lineWaiters = [] }
        cond.broadcast()   // readers waiting for room drop their lines
        for c in readers { c.resume(returning: nil) }
        if let transcript {
            var ending = exit.abandoned ? "abandoned" : exit.signal.map { "signal \($0)" } ?? "exit status \(exit.status)"
            if exit.stalled != nil { ending += ", stalled" }
            if exit.cancelled { ending += ", cancelled" }
            transcript.write("==== \(Instant.format(Self.now())) \(ending)")
            transcript.close()
        }
        let resume = waiters
        waiters = []
        for c in resume { c.resume(returning: exit) }
    }
}

/// The transcript file: appended to, one line at a time, from both reader threads. The first failure to open or write it
/// is kept as `problem` (process.noTranscript).
private final class Transcript: @unchecked Sendable {
    private let lock = NSLock()
    private let path: String
    private var fd: Int32
    private var failure: BroMessage?

    init(_ path: String) {
        self.path = path
        try? FileManager.default.createDirectory(atPath: (path as NSString).deletingLastPathComponent, withIntermediateDirectories: true)
        fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0o644)
        if fd < 0 { fail(String(cString: strerror(errno))) }
    }

    var problem: BroMessage? { lock.withLock { failure } }

    private func fail(_ reason: String) {
        if failure == nil { failure = BroMessage(.processNoTranscript, [("path", .string(path)), ("reason", .string(reason))], severity: .warning) }
    }

    func write(_ line: String) {
        lock.withLock {
            guard fd >= 0 else { return }
            let bytes = Array((line + "\n").utf8)
            let written = bytes.withUnsafeBytes { Darwin.write(fd, $0.baseAddress, $0.count) }
            if written != bytes.count { fail(written < 0 ? String(cString: strerror(errno)) : "a short write") }
        }
    }

    func close() {
        lock.withLock {
            if fd >= 0 { Darwin.close(fd) }
            fd = -1
        }
    }
}

/// Output bytes into lines, as every launcher splits them: at \n, \r or \r\n, empty lines dropped, UTF-8 with bad bytes
/// replaced. A line is cut after `maxBytes` bytes and ends with " [cut]"; the rest of it, up to the next line break, is
/// dropped, so a tool writing binary can't grow memory without limit.
struct LineSplitter {
    static let maxBytes = 65536
    static let cutMark = " [cut]"
    private var pending: [UInt8] = []
    private var discarding = false

    /// The lines `bytes` completes.
    mutating func feed(_ bytes: ArraySlice<UInt8>) -> [String] {
        var lines: [String] = []
        for b in bytes {
            if b == 0x0A || b == 0x0D {
                if !discarding && !pending.isEmpty { lines.append(String(decoding: pending, as: UTF8.self)) }
                pending.removeAll(keepingCapacity: true)
                discarding = false
            } else if !discarding {
                pending.append(b)
                if pending.count == Self.maxBytes {
                    lines.append(String(decoding: pending, as: UTF8.self) + Self.cutMark)
                    pending.removeAll(keepingCapacity: true)
                    discarding = true
                }
            }
        }
        return lines
    }

    /// The last line, when the output didn't end with a line break.
    func finish() -> String? { !discarding && !pending.isEmpty ? String(decoding: pending, as: UTF8.self) : nil }
}
