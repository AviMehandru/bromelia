import BroAdapters
import BroDomain
import BroFoundation
import BroPorts
import Foundation

/// A launcher that plays a script instead of running anything (plan §17's test kit): the lines, one at a time as they
/// are asked for; then the exit status, after creating a file in the destination. A stop ends it at once with status
/// -1 and signal 15. It records every spec it was given.
final class ScriptedProcessLauncher: ProcessLauncher, @unchecked Sendable {
    private let lock = NSLock()
    private(set) var started: [ProcessSpec] = []
    private(set) var last: Script?
    var lines: [String] = []
    /// The indexes of the lines that come on stderr (the others on stdout).
    var stderr: Set<Int> = []
    var exitCode = 0
    var writeFileIn: String?
    /// The files written in writeFileIn: a name with / makes its folders; "" writes writeFileIn itself as a file.
    var writeFileNames = ["title_t00.mkv"]
    /// After the lines: end as stopped for silence (status -1, signal 15, stalled for the spec's timeout).
    var stalls = false
    var afterLine: (@Sendable (Int) -> Void)?
    /// When it ends normally: create this many parts from the -o argument's %03d pattern (mkvmerge --split).
    var splitParts = 0
    /// When it ends normally: write this text to the file named by argument index (mkvextract's output).
    var writeArgument: (index: Int, text: String)?

    func start(_ spec: ProcessSpec) throws(BroError) -> any RunningProcess {
        var effects: [@Sendable () -> Void] = []
        if splitParts > 0, let o = spec.arguments.firstIndex(of: "-o") {
            let pattern = spec.arguments[o + 1], count = splitParts
            effects.append {
                for i in 1...count {
                    FileManager.default.createFile(atPath: pattern.replacingOccurrences(of: "%03d", with: String(format: "%03d", i)), contents: Data())
                }
            }
        }
        if let w = writeArgument {
            let path = spec.arguments[w.index], text = w.text
            effects.append { FileManager.default.createFile(atPath: path, contents: Data(text.utf8)) }
        }
        let script = Script(lines: lines, stderr: stderr, exitCode: exitCode, writeFileIn: writeFileIn, afterLine: afterLine, effects: effects)
        script.writeFileNames = writeFileNames
        script.stalledFor = stalls ? (spec.stallTimeout ?? Duration(seconds: 0)) : nil
        lock.withLock {
            started.append(spec)
            last = script
        }
        return script
    }

    final class Script: RunningProcess, @unchecked Sendable {
        private let lock = NSLock()
        private let script: [String]
        private let stderr: Set<Int>
        private let exitCode: Int
        private let writeFileIn: String?
        private let afterLine: (@Sendable (Int) -> Void)?
        private let effects: [@Sendable () -> Void]
        private var stoppedBy: StopReason?
        var writeFileNames = ["title_t00.mkv"]
        var stalledFor: Duration?
        private var handed = 0
        private var result: ProcessExit?
        private var waiters: [CheckedContinuation<ProcessExit, Never>] = []

        init(lines: [String], stderr: Set<Int> = [], exitCode: Int, writeFileIn: String?, afterLine: (@Sendable (Int) -> Void)?, effects: [@Sendable () -> Void] = []) {
            script = lines
            self.stderr = stderr
            self.effects = effects
            self.exitCode = exitCode
            self.writeFileIn = writeFileIn
            self.afterLine = afterLine
        }

        var stopReason: StopReason? { lock.withLock { stoppedBy } }
        var linesHanded: Int { lock.withLock { handed } }

        func lines() -> AsyncStream<OutputLine> {
            AsyncStream(unfolding: { [self] in next() })
        }

        private func next() -> OutputLine? {
            lock.lock()
            if stoppedBy == nil, handed > 0 { let n = handed; lock.unlock(); afterLine?(n); lock.lock() }
            if stoppedBy == nil && handed < script.count {
                let text = script[handed], stream: OutputSource = stderr.contains(handed) ? .stderr : .stdout
                handed += 1
                lock.unlock()
                return OutputLine(stream: stream, text: text, at: Instant(unixMilliseconds: 0))
            }
            let exit: ProcessExit
            if stoppedBy == nil, let stalledFor {
                exit = ProcessExit(status: -1, signal: 15, stalled: stalledFor)
            } else if let reason = stoppedBy {
                exit = ProcessExit(status: -1, signal: 15, cancelled: reason == .cancelled || reason == .shutdown)
            } else {
                if let dir = writeFileIn {
                    for name in writeFileNames {
                        let path = name.isEmpty ? dir : dir + "/" + name
                        try? FileManager.default.createDirectory(atPath: (path as NSString).deletingLastPathComponent, withIntermediateDirectories: true)
                        FileManager.default.createFile(atPath: path, contents: Data())
                    }
                }
                if result == nil { for e in effects { e() } }
                exit = ProcessExit(status: exitCode)
            }
            let resume = result == nil ? waiters : []
            if result == nil {
                result = exit
                waiters = []
            }
            lock.unlock()
            for c in resume { c.resume(returning: exit) }
            return nil
        }

        func wait() async -> ProcessExit {
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

        func stop(_ reason: StopReason) {
            lock.withLock { if stoppedBy == nil { stoppedBy = reason } }
        }
    }
}

/// An isolation that records what happens to its leases.
final class RecordingIsolation: SettingsIsolation, @unchecked Sendable {
    private let lock = NSLock()
    private var entries: [String] = []
    var log: [String] { lock.withLock { entries } }
    func add(_ s: String) { lock.withLock { entries.append(s) } }

    func prepare(_ settings: MakemkvRunSettings) throws(BroError) -> any IsolationLease {
        add("prepare")
        return Lease(owner: self, settings: settings)
    }

    struct Lease: IsolationLease {
        let owner: RecordingIsolation
        let settings: MakemkvRunSettings
        func environment() -> [String: String] { ["HOME": settings.workDirectory] }
        func profilePath() -> String? { settings.profileXml == nil ? nil : settings.workDirectory + "/profile.mmcp.xml" }
        func firstOutput() { owner.add("firstOutput") }
        func release() { owner.add("release") }
    }
}

struct FixedLocator: ToolLocator {
    let makemkvcon: String?
    func locate(_ tool: ToolKind) -> ToolInfo {
        if tool == .makemkvcon, let makemkvcon { return ToolInfo(tool: tool, path: makemkvcon, capabilities: []) }
        return ToolInfo(tool: tool, capabilities: [], why: BroMessage(.toolMissing, [("tool", .string(tool.rawValue))], severity: .warning))
    }
}

final class CountingSink: RunSink, @unchecked Sendable {
    private let lock = NSLock()
    private var n = 0
    var events: Int { lock.withLock { n } }
    func event(_ event: RobotEvent) { lock.withLock { n += 1 } }
}
