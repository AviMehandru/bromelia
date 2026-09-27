import Foundation

/// Runs a child process, streaming its combined stdout/stderr line by line.
final class ProcessRunner: @unchecked Sendable {
    struct Output: Sendable {
        var exitCode: Int32
        var wasCancelled: Bool
        var timedOut: Bool
    }

    let executable: URL
    let arguments: [String]
    let environment: [String: String]?
    let workingDirectory: URL?

    private let process = Process()
    private let lock = NSLock()
    private var cancelled = false
    private var timedOut = false

    init(executable: URL, arguments: [String], environment: [String: String]? = nil, workingDirectory: URL? = nil) {
        self.executable = executable
        self.arguments = arguments
        self.environment = environment
        self.workingDirectory = workingDirectory
    }

    var commandLine: String {
        ([executable.path] + arguments).map(ArgumentSplitter.quote).joined(separator: " ")
    }

    /// Runs the process to completion. `onLine` is called on a background queue for every output line.
    func run(timeout: TimeInterval = 0, onLine: @escaping @Sendable (String) -> Void) async throws -> Output {
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
        let readDone = DispatchSemaphore(value: 0)
        let readQueue = DispatchQueue(label: "bromelia.process.read")
        readQueue.async {
            while true {
                let data = handle.availableData
                if data.isEmpty { break }
                reader.feed(data)
            }
            reader.finish()
            readDone.signal()
        }

        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (cont: CheckedContinuation<Output, Error>) in
                process.terminationHandler = { p in
                    readDone.wait()
                    p.terminationHandler = nil
                    self.lock.lock()
                    let out = Output(exitCode: p.terminationStatus, wasCancelled: self.cancelled, timedOut: self.timedOut)
                    self.lock.unlock()
                    cont.resume(returning: out)
                }
                do {
                    try process.run()
                    // The write end is owned by the child now; close our copy so EOF is delivered.
                    try? pipe.fileHandleForWriting.close()
                } catch {
                    process.terminationHandler = nil
                    try? pipe.fileHandleForWriting.close()
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
            }
        } onCancel: {
            self.cancel()
        }
    }

    func cancel() {
        lock.lock(); cancelled = true; lock.unlock()
        terminate()
    }

    /// SIGINT, then SIGTERM after 5 s, then SIGKILL after 10 s.
    private func terminate() {
        guard process.isRunning else { return }
        let pid = process.processIdentifier
        kill(pid, SIGINT)
        DispatchQueue.global().asyncAfter(deadline: .now() + 5) { [weak self] in
            guard let self, self.process.isRunning else { return }
            kill(pid, SIGTERM)
            DispatchQueue.global().asyncAfter(deadline: .now() + 5) { [weak self] in
                guard let self, self.process.isRunning else { return }
                kill(pid, SIGKILL)
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
