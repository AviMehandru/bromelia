import Foundation

/// Runs a child process, streaming its combined stdout/stderr line by line.
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

    private let process = Process()
    private let lock = NSLock()
    private var cancelled = false
    private var timedOut = false
    private var stalled = false
    private var lastOutput = Date()
    private var finished = false
    private var finish: ((Output) -> Void)?

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

    /// Runs the process to completion. `onLine` is called on a background queue for every output line.
    /// With `stallTimeout`, the process is stopped when it prints nothing for that many seconds.
    func run(timeout: TimeInterval = 0, stallTimeout: TimeInterval = 0, onLine: @escaping @Sendable (String) -> Void) async throws -> Output {
        let pipe = Pipe()
        process.executableURL = executable
        process.arguments = arguments
        if let environment { process.environment = environment }
        if let workingDirectory { process.currentDirectoryURL = workingDirectory }
        process.standardOutput = pipe
        process.standardError = pipe
        process.standardInput = FileHandle.nullDevice

        let reader = LineReader(onLine: onLine)
        let handle = pipe.fileHandleForReading
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

        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (cont: CheckedContinuation<Output, Error>) in
                // The caller is resumed once: when the process ends, or when it is abandoned.
                self.lock.lock()
                self.finish = { cont.resume(returning: $0) }
                self.lock.unlock()
                process.terminationHandler = { p in
                    // Output normally ends with the process. A child it left behind may keep the pipe open,
                    // so don't wait for the end of output for long.
                    _ = readDone.wait(timeout: .now() + 5)
                    p.terminationHandler = nil
                    self.lock.lock()
                    let out = Output(exitCode: p.terminationStatus, wasCancelled: self.cancelled, timedOut: self.timedOut, stalled: self.stalled)
                    self.lock.unlock()
                    self.complete(out)
                }
                do {
                    try process.run()
                    // The write end is owned by the child now; close our copy so EOF is delivered.
                    try? pipe.fileHandleForWriting.close()
                } catch {
                    process.terminationHandler = nil
                    try? pipe.fileHandleForWriting.close()
                    self.lock.lock(); self.finish = nil; self.finished = true; self.lock.unlock()
                    cont.resume(throwing: error)
                    return
                }
                if timeout > 0 {
                    DispatchQueue.global().asyncAfter(deadline: .now() + timeout) { [weak self] in
                        guard let self, self.process.isRunning else { return }
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
            guard let self, self.process.isRunning else { return }
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

    /// The stop signal, then SIGTERM after 5 s, then SIGKILL after 10 s. A process that is still there
    /// 30 s after SIGKILL (stuck in the kernel, e.g. on a hung drive) is abandoned so the job can end.
    private func terminate() {
        guard process.isRunning else { return }
        let pid = process.processIdentifier
        kill(pid, stopSignal)
        DispatchQueue.global().asyncAfter(deadline: .now() + 5) { [weak self] in
            guard let self, self.process.isRunning else { return }
            kill(pid, SIGTERM)
            DispatchQueue.global().asyncAfter(deadline: .now() + 5) { [weak self] in
                guard let self, self.process.isRunning else { return }
                kill(pid, SIGKILL)
                DispatchQueue.global().asyncAfter(deadline: .now() + 30) { [weak self] in
                    guard let self, self.process.isRunning else { return }
                    self.lock.lock()
                    let out = Output(exitCode: -1, wasCancelled: self.cancelled, timedOut: self.timedOut, stalled: self.stalled, abandoned: true)
                    self.lock.unlock()
                    self.complete(out)
                }
            }
        }
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
