import Foundation

/// Runs a child process, streaming its combined stdout/stderr line by line.
///
/// The process starts (posix_spawn) in a process group of its own, and stopping it signals the whole group, so what it
/// started stops too (a shell's background child would otherwise go on). Once a stopped process has ended, whatever is
/// left of its group is killed; what a process that ended by itself left running is left alone. The process is
/// reaped only after that, so a signal never reaches a pid used again.
final class ProcessRunner: @unchecked Sendable {
    struct Output: Sendable {
        var exitCode: Int32
        var wasCancelled: Bool
        var timedOut: Bool
        /// Stopped because it printed nothing for `stallTimeout` seconds.
        var stalled = false
        /// It didn't exit even after SIGKILL (e.g. stuck in a drive I/O call); we stopped waiting for it.
        var abandoned = false
    }

    let executable: URL
    let arguments: [String]
    let environment: [String: String]?
    let workingDirectory: URL?
    /// Signal sent first when stopping the process (SIGTERM follows after 5 s, SIGKILL after 10 s).
    /// makemkvcon ignores SIGINT, so it is stopped with SIGTERM.
    let stopSignal: Int32

    private let lock = NSLock()
    private var pid: pid_t = 0
    private var cancelled = false
    private var timedOut = false
    private var stalled = false
    private var stopping = false
    private var lastOutput = Date()
    private var finished = false
    private var finish: ((Output) -> Void)?
    private var exited = false

    init(executable: URL, arguments: [String], environment: [String: String]? = nil, workingDirectory: URL? = nil,
         stopSignal: Int32 = SIGINT) {
        self.executable = executable
        self.arguments = arguments
        self.environment = environment
        self.workingDirectory = workingDirectory
        self.stopSignal = stopSignal
    }

    var commandLine: String {
        ([executable.path] + arguments).map(ArgumentSplitter.quote).joined(separator: " ")
    }

    /// Whether the process has started and not ended yet.
    private var isRunning: Bool { lock.withLock { pid > 0 && !exited } }

    /// Runs the process to completion. `onLine` is called on a background queue for every output line.
    /// With `stallTimeout`, the process is stopped when it prints nothing for that many seconds.
    func run(timeout: TimeInterval = 0, stallTimeout: TimeInterval = 0, onLine: @escaping @Sendable (String) -> Void) async throws -> Output {
        let child = try spawn()
        let reader = LineReader(onLine: onLine)
        let handle = FileHandle(fileDescriptor: child.output, closeOnDealloc: true)
        lastOutput = Date()
        let readDone = DispatchSemaphore(value: 0)
        let readQueue = DispatchQueue(label: "bromelia.process.read")
        readQueue.async {
            while true {
                let data = handle.availableData
                if data.isEmpty { break }
                self.lock.lock(); self.lastOutput = Date(); self.lock.unlock()
                reader.feed(data)
            }
            reader.finish()
            readDone.signal()
        }

        return await withTaskCancellationHandler {
            await withCheckedContinuation { (cont: CheckedContinuation<Output, Never>) in
                // The caller is resumed once: when the process ends, or when it is abandoned.
                self.lock.lock()
                self.finish = { cont.resume(returning: $0) }
                self.lock.unlock()
                let watcher = Lifeline.watch(child.pid)
                Thread.detachNewThread {
                    let status = self.wait(child.pid, watcher: watcher)
                    // Output normally ends with the process. A child it left behind may keep the pipe open,
                    // so don't wait for the end of output for long.
                    _ = readDone.wait(timeout: .now() + 5)
                    self.lock.lock()
                    let out = Output(exitCode: status, wasCancelled: self.cancelled, timedOut: self.timedOut, stalled: self.stalled)
                    self.lock.unlock()
                    self.complete(out)
                }
                if timeout > 0 {
                    DispatchQueue.global().asyncAfter(deadline: .now() + timeout) { [weak self] in
                        guard let self, self.isRunning else { return }
                        self.lock.lock(); self.timedOut = true; self.lock.unlock()
                        self.terminate()
                    }
                }
                if stallTimeout > 0 { self.watchForStall(stallTimeout) }
            }
        } onCancel: {
            self.cancel()
        }
    }

    func cancel() {
        lock.lock(); cancelled = true; lock.unlock()
        terminate()
    }

    /// Starts the process in a process group of its own, stdin /dev/null, stdout and stderr into one pipe; its pid and
    /// the pipe's read end. Default signal handling and an empty mask whatever Bromelia has, and only those descriptors.
    private func spawn() throws -> (pid: pid_t, output: Int32) {
        var fds: [Int32] = [-1, -1]
        guard pipe(&fds) == 0 else { throw Self.error(errno, "Couldn't start \(executable.path)") }
        _ = fcntl(fds[0], F_SETFD, FD_CLOEXEC)
        var actions: posix_spawn_file_actions_t?
        posix_spawn_file_actions_init(&actions)
        defer { posix_spawn_file_actions_destroy(&actions) }
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0)
        posix_spawn_file_actions_adddup2(&actions, fds[1], 1)
        posix_spawn_file_actions_adddup2(&actions, fds[1], 2)
        if let workingDirectory { posix_spawn_file_actions_addchdir_np(&actions, workingDirectory.path) }
        var attr: posix_spawnattr_t?
        posix_spawnattr_init(&attr)
        defer { posix_spawnattr_destroy(&attr) }
        posix_spawnattr_setpgroup(&attr, 0)
        var defaults = sigset_t(), empty = sigset_t()
        sigfillset(&defaults)
        sigemptyset(&empty)
        posix_spawnattr_setsigdefault(&attr, &defaults)
        posix_spawnattr_setsigmask(&attr, &empty)
        posix_spawnattr_setflags(&attr, Int16(POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_CLOEXEC_DEFAULT))
        let env = environment ?? ProcessInfo.processInfo.environment
        let argv = ([executable.path] + arguments).map { strdup($0) } + [nil]
        let envp = env.map { strdup("\($0.key)=\($0.value)") } + [nil]
        defer {
            argv.forEach { free($0) }
            envp.forEach { free($0) }
        }
        var child: pid_t = 0
        let rc = posix_spawn(&child, executable.path, &actions, &attr, argv, envp)
        close(fds[1])
        guard rc == 0 else {
            close(fds[0])
            throw Self.error(rc, "Couldn't start \(executable.path)")
        }
        lock.withLock { pid = child }
        return (child, fds[0])
    }

    private static func error(_ code: Int32, _ what: String) -> NSError {
        NSError(domain: NSPOSIXErrorDomain, code: Int(code), userInfo: [NSLocalizedDescriptionKey: "\(what): \(String(cString: strerror(code)))"])
    }

    /// Waits for the process to end without reaping it, so its pid (and process group) can't be used again while a
    /// signal may still be sent; ends its Lifeline watcher; kills what is left of a stopped process's group; then reaps
    /// it. Its exit status, or the signal that ended it.
    private func wait(_ child: pid_t, watcher: pid_t?) -> Int32 {
        var info = siginfo_t()
        while waitid(P_PID, id_t(child), &info, WEXITED | WNOWAIT) == -1 && errno == EINTR {}
        if let watcher { Lifeline.end(watcher) }
        lock.lock()
        exited = true
        if stopping { kill(-child, SIGKILL) }
        lock.unlock()
        var status: Int32 = 0
        while waitpid(child, &status, 0) == -1 && errno == EINTR {}
        return status & 0x7f == 0 ? (status >> 8) & 0xff : status & 0x7f
    }

    private func complete(_ out: Output) {
        lock.lock()
        let f = finished ? nil : finish
        finished = true
        finish = nil
        lock.unlock()
        f?(out)
    }

    /// Checks every few seconds whether the process has printed anything within `limit` seconds.
    private func watchForStall(_ limit: TimeInterval) {
        DispatchQueue.global().asyncAfter(deadline: .now() + min(limit, 5)) { [weak self] in
            guard let self, self.isRunning else { return }
            self.lock.lock()
            let quiet = Date().timeIntervalSince(self.lastOutput)
            let done = self.finished
            if quiet >= limit { self.stalled = true }
            let stop = self.stalled
            self.lock.unlock()
            if done { return }
            if stop { self.terminate() } else { self.watchForStall(limit) }
        }
    }

    /// Sends `sig` to the process's group while the process hasn't ended; false once it has.
    @discardableResult
    private func signal(_ sig: Int32) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        guard pid > 0, !exited else { return false }
        if kill(-pid, sig) != 0 { kill(pid, sig) }
        return true
    }

    /// The stop signal, then SIGTERM after 5 s, then SIGKILL after 10 s, each to the whole group. A process that is
    /// still there 30 s after SIGKILL (stuck in the kernel, e.g. on a hung drive) is abandoned so the job can end.
    private func terminate() {
        lock.withLock { if pid > 0 && !exited { stopping = true } }
        guard signal(stopSignal) else { return }
        DispatchQueue.global().asyncAfter(deadline: .now() + 5) { [weak self] in
            guard let self, self.signal(SIGTERM) else { return }
            DispatchQueue.global().asyncAfter(deadline: .now() + 5) { [weak self] in
                guard let self, self.signal(SIGKILL) else { return }
                DispatchQueue.global().asyncAfter(deadline: .now() + 30) { [weak self] in
                    guard let self, self.isRunning else { return }
                    self.lock.lock()
                    let out = Output(exitCode: -1, wasCancelled: self.cancelled, timedOut: self.timedOut, stalled: self.stalled, abandoned: true)
                    self.lock.unlock()
                    self.complete(out)
                }
            }
        }
    }
}

/// So that a crash (or kill -9) of Bromelia leaves no tools behind, each process gets a watcher: /bin/sh in its own
/// process group, reading a pipe that nothing writes to. Bromelia holds the pipe's write end (close-on-exec, never
/// closed); when Bromelia ends, however it ends, the kernel closes it, the watcher's read returns, and it stops the
/// process's group: TERM, then KILL if the group is still there 5 s later. The watcher stops as soon as the group is
/// gone, so it never signals one used again. When the process ends first, its watcher is killed and reaped before the
/// process is reaped. (makemkvcon ignores SIGPIPE, so without this it went on reading the disc into a folder recovery
/// had marked INCOMPLETE.)
enum Lifeline {
    static let script = #"read -r _; kill -s TERM -- "-$1" 2>/dev/null || exit 0; i=0; while [ $i -lt 50 ]; do sleep 0.1; kill -0 -- "-$1" 2>/dev/null || exit 0; i=$((i+1)); done; kill -s KILL -- "-$1" 2>/dev/null"#

    /// The pipe's read end; -1 when it couldn't be made.
    static let readEnd: Int32 = {
        var fds: [Int32] = [-1, -1]
        guard pipe(&fds) == 0 else { return -1 }
        _ = fcntl(fds[0], F_SETFD, FD_CLOEXEC)
        _ = fcntl(fds[1], F_SETFD, FD_CLOEXEC)
        return fds[0]   // fds[1] stays open until Bromelia ends
    }()

    /// Starts the watcher of process group `pid`, reading `readEnd`; nil when it couldn't start (a crash then leaves the
    /// process running).
    static func watch(_ pid: pid_t, readEnd: Int32 = readEnd) -> pid_t? {
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
        posix_spawnattr_setpgroup(&attr, 0)   // out of Bromelia's group: a terminal's ^C doesn't reach it
        var defaults = sigset_t(), empty = sigset_t()
        sigfillset(&defaults)
        sigemptyset(&empty)
        posix_spawnattr_setsigdefault(&attr, &defaults)
        posix_spawnattr_setsigmask(&attr, &empty)
        posix_spawnattr_setflags(&attr, Int16(POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_CLOEXEC_DEFAULT))
        let words = ["/bin/sh", "-c", script, "bromelia-lifeline", String(pid)], environment = ["PATH=/usr/bin:/bin"]
        let argv = words.map { strdup($0) } + [nil]
        let envp = environment.map { strdup($0) } + [nil]
        defer {
            argv.forEach { free($0) }
            envp.forEach { free($0) }
        }
        var watcher: pid_t = 0
        return posix_spawn(&watcher, "/bin/sh", &actions, &attr, argv, envp) == 0 ? watcher : nil
    }

    /// Kills and reaps a watcher whose process has ended.
    static func end(_ watcher: pid_t) {
        kill(watcher, SIGKILL)
        var status: Int32 = 0
        while waitpid(watcher, &status, 0) == -1 && errno == EINTR {}
    }
}

/// Splits a byte stream into UTF-8 lines (\n or \r terminated).
final class LineReader: @unchecked Sendable {
    private var buffer = Data()
    private let onLine: @Sendable (String) -> Void

    init(onLine: @escaping @Sendable (String) -> Void) { self.onLine = onLine }

    func feed(_ data: Data) {
        buffer.append(data)
        while let idx = buffer.firstIndex(where: { $0 == 0x0A || $0 == 0x0D }) {
            let lineData = buffer[buffer.startIndex..<idx]
            buffer.removeSubrange(buffer.startIndex...idx)
            if !lineData.isEmpty { emit(lineData) }
        }
    }

    func finish() {
        if !buffer.isEmpty { emit(buffer); buffer.removeAll() }
    }

    private func emit(_ d: Data) {
        onLine(String(decoding: d, as: UTF8.self))
    }
}
