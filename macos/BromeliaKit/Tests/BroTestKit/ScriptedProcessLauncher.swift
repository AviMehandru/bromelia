import BroDomain
import BroFoundation
import BroPorts
import Foundation

/// A launcher that plays a script instead of running anything (plan §17's test kit): the lines, one at a time as they
/// are asked for; then the exit status, after creating a file in the destination. A stop ends it at once with status
/// -1 and signal 15. It records every spec it was given. The files it creates go to real disk, or through `files` when it
/// has one (the test root's memory file system).
public final class ScriptedProcessLauncher: ProcessLauncher, @unchecked Sendable {
    private let lock = NSLock()
    /// Where it creates its output files (nil: real disk).
    public let files: (any FileSystem)?
    /// The lines for each spec (nil, or a nil answer: `lines`), e.g. a drive scan against a rip.
    public var linesFor: (@Sendable (ProcessSpec) -> [String]?)?
    /// A spec this answers true for runs until it is stopped after its lines (a rip in progress).
    public var runsUntilStopped: (@Sendable (ProcessSpec) -> Bool)?
    /// How long a stopped process takes to end (a process slow to die); real time.
    public var stopTakes = Swift.Duration.zero
    private var heldCount = 0
    private var runningCount = 0
    public private(set) var started: [ProcessSpec] = []
    public private(set) var last: Script?
    public var lines: [String] = []
    /// The indexes of the lines that come on stderr (the others on stdout).
    public var stderr: Set<Int> = []
    public var exitCode = 0
    public var writeFileIn: String?
    /// The files written in writeFileIn: a name with / makes its folders; "" writes writeFileIn itself as a file.
    public var writeFileNames = ["title_t00.mkv"]
    /// After the lines: end as stopped for silence (status -1, signal 15, stalled for the spec's timeout).
    public var stalls = false
    public var afterLine: (@Sendable (Int) -> Void)?
    /// When it ends normally: create this many parts from the -o argument's %03d pattern (mkvmerge --split).
    public var splitParts = 0
    /// When it ends normally: write this text to the file named by argument index (mkvextract's output).
    public var writeArgument: (index: Int, text: String)?
    /// The process reports process.noTranscript for the spec's transcript.
    public var transcriptFails = false

    public init(files: (any FileSystem)? = nil) { self.files = files }

    public func start(_ spec: ProcessSpec) throws(BroError) -> any RunningProcess {
        var effects: [@Sendable () -> Void] = []
        if splitParts > 0, let o = spec.arguments.firstIndex(of: "-o") {
            let pattern = spec.arguments[o + 1], count = splitParts
            effects.append { [self] in
                for i in 1...count { writeText(pattern.replacingOccurrences(of: "%03d", with: String(format: "%03d", i)), "") }
            }
        }
        if let w = writeArgument {
            let path = spec.arguments[w.index], text = w.text
            effects.append { [self] in writeText(path, text) }
        }
        let script = Script(lines: linesFor?(spec) ?? lines, stderr: stderr, exitCode: exitCode, writeFileIn: writeFileIn, afterLine: afterLine,
                            effects: effects)
        script.writeFileNames = writeFileNames
        script.transcriptFailsAt = transcriptFails ? spec.transcript ?? "" : nil
        script.stalledFor = stalls ? (spec.stallTimeout ?? Duration(seconds: 0)) : nil
        script.owner = self
        script.holds = runsUntilStopped?(spec) ?? false
        lock.withLock {
            runningCount += 1
            started.append(spec)
            last = script
        }
        return script
    }

    /// The processes waiting to be stopped after their lines (`runsUntilStopped`): each is a step a harness knows is
    /// parked.
    public func held() -> Int { lock.withLock { heldCount } }

    /// The processes that haven't ended yet.
    public func running() -> Int { lock.withLock { runningCount } }

    /// The specs started so far (any thread).
    public func specs() -> [ProcessSpec] { lock.withLock { started } }

    fileprivate func holding(_ by: Int) { lock.withLock { heldCount += by } }

    fileprivate func ended() { lock.withLock { runningCount -= 1 } }

    fileprivate func writeText(_ path: String, _ text: String) {
        if let files {
            // the test root's paths use '/' everywhere
            let folder = path.lastIndex(of: "/").map { String(path[..<$0]) } ?? path
            try? files.createDirectory(folder, parentsMustExist: false)
            try? files.writeAtomically(path, bytes: Array(text.utf8), mode: 0o644)
            return
        }
        try? FileManager.default.createDirectory(atPath: (path as NSString).deletingLastPathComponent, withIntermediateDirectories: true)
        _ = FileManager.default.createFile(atPath: path, contents: Data(text.utf8))
    }

    public final class Script: RunningProcess, @unchecked Sendable {
        private let lock = NSLock()
        private let script: [String]
        private let stderr: Set<Int>
        private let exitCode: Int
        private let writeFileIn: String?
        private let afterLine: (@Sendable (Int) -> Void)?
        private let effects: [@Sendable () -> Void]
        private var stoppedBy: StopReason?
        public var writeFileNames = ["title_t00.mkv"]
        public var stalledFor: Duration?
        public var transcriptFailsAt: String?
        /// The launcher that started it: its counts, its file system, its stop time.
        fileprivate var owner: ScriptedProcessLauncher?
        /// Runs until stopped after its lines.
        fileprivate var holds = false
        private var hold: CheckedContinuation<Void, Never>?
        private var handed = 0
        private var result: ProcessExit?
        private var waiters: [CheckedContinuation<ProcessExit, Never>] = []

        public init(lines: [String], stderr: Set<Int> = [], exitCode: Int, writeFileIn: String?, afterLine: (@Sendable (Int) -> Void)?, effects: [@Sendable () -> Void] = []) {
            script = lines
            self.stderr = stderr
            self.effects = effects
            self.exitCode = exitCode
            self.writeFileIn = writeFileIn
            self.afterLine = afterLine
        }

        public var stopReason: StopReason? { lock.withLock { stoppedBy } }

        public func transcriptProblem() -> BroMessage? {
            transcriptFailsAt.map { BroMessage(.processNoTranscript, [("path", .string($0)), ("reason", .string("No space left on device"))], severity: .warning) }
        }
        public var linesHanded: Int { lock.withLock { handed } }

        public func lines() -> AsyncStream<OutputLine> {
            AsyncStream(unfolding: { [self] in
                switch next() {
                case .line(let line): return line
                case .ended: return nil
                case .held:
                    await waitForStop()
                    if case .line(let line) = next() { return line }
                    return nil
                }
            })
        }

        /// A held process, its lines given and not stopped yet, waits here until it is stopped.
        private func waitForStop() async {
            guard let owner, lock.withLock({ holds && stoppedBy == nil && handed >= script.count && result == nil }) else { return }
            owner.holding(1)
            await withCheckedContinuation { (c: CheckedContinuation<Void, Never>) in
                let now = lock.withLock { () -> Bool in
                    if stoppedBy != nil || result != nil { return true }
                    hold = c
                    return false
                }
                if now { c.resume() }
            }
            owner.holding(-1)
        }

        private enum Next {
            case line(OutputLine)
            /// Its lines given, it runs until stopped: the caller waits for the stop and asks again.
            case held
            case ended
        }

        private func next() -> Next {
            lock.lock()
            if stoppedBy == nil, handed > 0 { let n = handed; lock.unlock(); afterLine?(n); lock.lock() }
            if stoppedBy == nil && handed < script.count {
                let text = script[handed], stream: OutputSource = stderr.contains(handed) ? .stderr : .stdout
                handed += 1
                lock.unlock()
                return .line(OutputLine(stream: stream, text: text, at: Instant(unixMilliseconds: 0)))
            }
            if stoppedBy == nil && holds && result == nil && owner != nil {
                lock.unlock()
                return .held
            }
            let exit: ProcessExit
            if stoppedBy == nil, let stalledFor {
                exit = ProcessExit(status: -1, signal: 15, stalled: stalledFor)
            } else if let reason = stoppedBy {
                exit = ProcessExit(status: -1, signal: 15, cancelled: reason == .cancelled || reason == .shutdown)
            } else {
                if let dir = writeFileIn {
                    for name in writeFileNames { writeFile(name.isEmpty ? dir : dir + "/" + name) }
                }
                if result == nil { for e in effects { e() } }
                exit = ProcessExit(status: exitCode)
            }
            finishLocked(exit)
            return .ended
        }

        /// An empty file, written while the lock is held (as before the test kit had a file system).
        private func writeFile(_ path: String) {
            if let owner {
                owner.writeText(path, "")
            } else {
                try? FileManager.default.createDirectory(atPath: (path as NSString).deletingLastPathComponent, withIntermediateDirectories: true)
                _ = FileManager.default.createFile(atPath: path, contents: Data())
            }
        }

        /// Ends it with `exit` unless it has ended (a stop that took time).
        private func finishLater(_ exit: ProcessExit) {
            lock.lock()
            finishLocked(exit)
        }

        /// Ends it with `exit` unless it has ended; unlocks.
        private func finishLocked(_ exit: ProcessExit) {
            let first = result == nil
            let resume = first ? waiters : []
            let held = hold
            if first {
                result = exit
                waiters = []
                hold = nil
            }
            lock.unlock()
            if first { owner?.ended() }
            held?.resume()
            for c in resume { c.resume(returning: exit) }
        }

        public func wait() async -> ProcessExit {
            await withCheckedContinuation { c in
                lock.lock()
                if let result {
                    lock.unlock()
                    c.resume(returning: result)
                } else {
                    waiters.append(c)
                    lock.unlock()
                }
            }
        }

        /// As a real process: it ends when stopped, whether or not anyone reads the rest of its lines.
        public func stop(_ reason: StopReason) {
            lock.lock()
            if stoppedBy == nil { stoppedBy = reason }
            let exit = ProcessExit(status: -1, signal: 15, cancelled: stoppedBy == .cancelled || stoppedBy == .shutdown)
            if let takes = owner?.stopTakes, takes > .zero {
                lock.unlock()
                Task {
                    try? await Task.sleep(for: takes)
                    self.finishLater(exit)
                }
                return
            }
            finishLocked(exit)
        }
    }
}
