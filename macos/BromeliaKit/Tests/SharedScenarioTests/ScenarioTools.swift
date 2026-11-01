import BroDomain
import BroFoundation
import BroPorts
import BroTestKit
import BroTestSupport
import Foundation

/// The scripted tools (decision 236): a ProcessLauncher that plays the world's tool scripts (shared/scenarios/README.md
/// "Tool scripts") instead of starting anything. The tool is the executable's name. makemkvcon's drive scan
/// (`info disc:9999`) is answered from the drive world; a makemkvcon backup whose destination exists is refused as the real
/// one refuses it; otherwise the first rule whose `when` matches writes its files, then prints its lines at once. It keeps
/// the ProcessLauncher contract the way the platform launchers do, on the ManualClock: a rule's silence, the stall watchdog
/// (spec.stallTimeout after the last line), the stop escalation (TERM first, or INT first: then TERM, then KILL, 5 s apart;
/// a signal the script ignores does nothing; makemkvcon ignores SIGINT), a process that outlives SIGKILL abandoned 30 s
/// later, and the transcript (the platform launchers' header, every line and the ending). Every launch but the drive scans
/// is recorded (`calls`), and each one inside a step reports the crash point inStep:<kind>:<n> (the step's n-th process) to
/// `crash` once its rule's files are written, before any line. Like the real makemkvcon, the line `saved` of a run that
/// wrote titles reports how many it wrote (lines.yaml's says 1; owner question H3a-1).
final class ScenarioTools: ProcessLauncher, @unchecked Sendable {
    private static let graceMs: Int64 = 5000
    private static let abandonMs: Int64 = 30_000

    private let world: ScenarioWorld
    fileprivate let clock: ManualClock
    fileprivate let fs: MemoryFileSystem
    private let named: [String: String]
    private let lock = NSLock()
    private var recorded: [String] = []
    private var alive: [Process] = []
    /// The backups the tools made (the destination's folder → the disc backed up): reading anything there reads that disc,
    /// whatever the engine renamed the backup to (an image for a folder, a folder for an image).
    private var backups: [(folder: String, disc: JsonValue)] = []
    private var step: String?
    private var inStep = 0
    private var crashHook: (@Sendable (String) throws(BroError) -> Void)?

    /// Hears inStep:<kind>:<n> before a process starts; what it throws, start throws (a kill).
    var crash: (@Sendable (String) throws(BroError) -> Void)? {
        get {
            lock.lock()
            defer { lock.unlock() }
            return crashHook
        }
        set {
            lock.lock()
            crashHook = newValue
            lock.unlock()
        }
    }

    init(world: ScenarioWorld, clock: ManualClock, fs: MemoryFileSystem) throws {
        self.world = world
        self.clock = clock
        self.fs = fs
        named = try ScenarioCatalog.lines()
    }

    /// A step of this kind begins (its beforeStep point): the processes started from now on are its.
    func stepBegins(_ kind: String?) {
        lock.withLock {
            step = kind
            inStep = 0
        }
    }

    /// Every launch but the drive scans, as "<tool> <arguments joined by spaces>", of one tool.
    func calls(_ tool: String) -> [String] { lock.withLock { recorded.filter { $0.hasPrefix(tool + " ") || $0 == tool } } }

    /// The processes that haven't ended and whose lines have all been read: each one parks the step's worker that waits for
    /// it (it ends only when the clock moves or it is stopped).
    func parked() -> Int {
        let processes = lock.withLock { alive }
        return processes.filter(\.drained).count
    }

    /// A kill: every process ends at once, without another line or transcript line.
    func endAll() {
        let processes = lock.withLock { () -> [Process] in
            let all = alive
            alive = []
            return all
        }
        for p in processes { p.killed() }
    }

    func start(_ spec: ProcessSpec) throws(BroError) -> any RunningProcess {
        var tool = Self.lastPart(spec.executable)
        if tool.lowercased().hasSuffix(".exe") { tool = String(tool.dropLast(4)) }
        if tool == "makemkvcon" && spec.arguments.contains("disc:9999") {
            return begin(Process(tools: self, spec: spec, run: Run(stdout: world.driveScan(), stderr: [], exit: 0, silence: nil, ignore: nil)))
        }
        let point = lock.withLock { () -> String? in
            recorded.append(([tool] + spec.arguments).joined(separator: " "))
            guard let step else { return nil }
            inStep += 1
            return "inStep:\(step):\(inStep)"
        }
        guard let script = world.script(tool) else {
            throw BroError(MessageCode.wire(.processCouldNotStart), [("program", .string(spec.executable)), ("reason", .string("no script"))])
        }
        // The process has started (its rule's files are written at its start) when the engine dies at the point.
        let run: Run
        do {
            run = try plan(tool, spec, script)
        } catch let error as BroError {
            throw error
        } catch {
            throw BroError("internal.unexpected", [("detail", .string("the script of \(tool): \(error)"))])
        }
        if let point { try crash?(point) }
        return begin(Process(tools: self, spec: spec, run: run))
    }

    private func begin(_ process: Process) -> Process {
        lock.withLock { alive.append(process) }
        process.begin()
        return process
    }

    fileprivate func ended(_ process: Process) { lock.withLock { alive.removeAll { $0 === process } } }

    // MARK: What a launch does

    /// What a rule makes a launch do: its lines (stdout, stderr), its exit (nil: never), its silence before that, the
    /// signals it ignores.
    fileprivate struct Run {
        var stdout: [String]
        var stderr: [String]
        var exit: Int?
        var silence: Double?
        var ignore: [String]?
    }

    /// The arguments of a makemkvcon run: its command (info, mkv, backup, …), source, title and destination.
    private struct Args {
        var command: String
        var source: String
        var title: String
        var dest: String
        var minLength: Int?
    }

    private static func makemkvArgs(_ a: [String]) -> Args {
        let minLength = a.first { $0.hasPrefix("--minlength=") }.flatMap { Int($0.dropFirst(12)) }
        guard let at = a.firstIndex(where: { ["info", "mkv", "backup", "reg", "f"].contains($0) }) else {
            return Args(command: a.first ?? "", source: "", title: "", dest: "", minLength: minLength)
        }
        let rest = a[(at + 1)...].filter { $0 != "--decrypt" }
        func arg(_ i: Int) -> String { i < rest.count ? rest[rest.startIndex + i] : "" }
        switch a[at] {
        case "mkv": return Args(command: "mkv", source: arg(0), title: arg(1), dest: arg(2), minLength: minLength)
        case "backup": return Args(command: "backup", source: arg(0), title: "", dest: arg(1), minLength: minLength)
        default: return Args(command: a[at], source: arg(0), title: "", dest: "", minLength: minLength)
        }
    }

    private func plan(_ tool: String, _ spec: ProcessSpec, _ script: JsonValue) throws -> Run {
        let a = spec.arguments
        let mk = tool == "makemkvcon" ? Self.makemkvArgs(a) : nil
        let command = mk?.command ?? a.first ?? ""
        let input = Self.option(a, "-i") ?? (command == "-J" && a.count > 1 ? a[1] : nil)
        let output = Self.option(a, "-o")
        let dest = mk?.dest ?? output ?? spec.workingDirectory ?? TestCompositionRoot.dataDir
        let debugLog = (spec.workingDirectory ?? TestCompositionRoot.dataDir) + "/MakeMKV_log.txt"
        let world = world
        func fill(_ text: String, _ title: String = "") -> String {
            var t = text.replacingOccurrences(of: "{title}", with: !title.isEmpty ? title : mk?.title ?? "")
                .replacingOccurrences(of: "{dest}", with: dest).replacingOccurrences(of: "{debugLog}", with: debugLog)
                .replacingOccurrences(of: "{device}", with: mk.map { world.deviceOf($0.source) } ?? "")
                .replacingOccurrences(of: "{input}", with: input ?? "").replacingOccurrences(of: "{output}", with: output ?? "")
            for (i, arg) in a.enumerated() { t = t.replacingOccurrences(of: "{arg\(i)}", with: arg) }
            for (name, value) in spec.environment { t = t.replacingOccurrences(of: "{env:\(name)}", with: value) }
            return t
        }

        var stdout: [String] = []
        var stderr: [String] = []
        if let mk, mk.command == "backup", fs.exists(mk.dest) {
            for name in ["backupFolderExists", "backupFailed", "backupFailedSummary"] { stdout.append(fill(named[name] ?? "")) }
            return Run(stdout: stdout, stderr: stderr, exit: 0, silence: nil, ignore: nil)
        }
        guard let rule = (script["rules"]?.array ?? []).first(where: { Self.matches($0["when"], command, mk, a, input) }) else {
            stderr.append("\(tool): no rule of its script matches \(a.joined(separator: " "))")
            return Run(stdout: stdout, stderr: stderr, exit: 127, silence: nil, ignore: nil)
        }
        let disc = mk != nil ? discOf(mk!.source) : world.disc
        if let mk, mk.command == "backup", let disc {
            let folder = mk.dest.utf8.lastIndex(of: UInt8(ascii: "/")).map { String(decoding: mk.dest.utf8[..<$0], as: UTF8.self) } ?? ""
            lock.withLock {
                backups.removeAll { $0.folder == folder }
                backups.append((folder, disc))
            }
        }

        if let debugText = rule["writeDebugLog"]?.string { write(debugLog, Array(debugText.utf8)) }
        var written: [Int]?
        if let wt = rule["writeTitles"], let mk, let disc {
            let count = ScenarioWorld.lines(try ScenarioWorld.listing(disc, minLength: mk.minLength))
                .filter { $0.hasPrefix("TINFO:") && $0.contains(",27,0,") }.count
            let only = wt["titles"]?.array.map { Set($0.compactMap { $0.int.map { Int($0) } }) }
            let titles: [Int] = mk.title == "all" ? (0..<count).filter { only == nil || only!.contains($0) } : Int(mk.title).map { [$0] } ?? []
            written = titles
            for i in titles { write(dest + "/" + String(format: "title_t%02d.mkv", i), Array(fill(wt["content"]?.string ?? "", String(i)).utf8)) }
        }
        for w in rule["write"]?.array ?? [] {
            var path = fill(w["path"]?.string ?? "")
            if !path.hasPrefix("/") { path = dest + "/" + path }
            let bytes: [UInt8]
            if w["copyOf"]?.string == "input", let input, fs.exists(input) {
                bytes = try fs.read(input)
            } else if let b = w["bytes"] {
                bytes = ScenarioWorld.fileBytes(b)
            } else {
                bytes = Array(fill(w["content"]?.string ?? "").utf8)
            }
            write(path, bytes)
        }
        if let iso = rule["writeIso"] { write(dest, ScenarioWorld.fileBytes(iso)) }
        for (path, content) in rule["writeFolder"]?.members ?? [] { write(dest + "/" + path, Array(fill(content.string ?? "").utf8)) }

        for line in rule["print"]?.array ?? [] {
            let text = line.string ?? ""
            if text == "listing" {
                if let disc { stdout += ScenarioWorld.lines(try ScenarioWorld.listing(disc, minLength: mk?.minLength)) }
            } else if text.hasPrefix("fixture:") {
                stdout += ScenarioWorld.lines(try Fixtures.text(String(text.dropFirst(8))), trimCR: true)
            } else if text == "saved", let written, written.count != 1 {
                stdout.append((named[text] ?? "").replacingOccurrences(of: "Copy complete. 1 titles", with: "Copy complete. \(written.count) titles")
                    .replacingOccurrences(of: ",\"1\"", with: ",\"\(written.count)\""))
            } else {
                stdout.append(fill(named[text] ?? text))
            }
        }
        for line in rule["stderr"]?.array ?? [] { stderr.append(fill(line.string ?? "")) }
        if let probe = rule["probe"], let input {
            for (glob, answer) in probe.members ?? [] where Self.glob(glob, Self.lastPart(input)) {
                stdout.append(Self.probeJson(answer["durationSeconds"]?.double ?? 0, (answer["tracks"]?.array ?? []).compactMap(\.string),
                                             Int(answer["chapters"]?.int ?? 0)))
                break
            }
        }
        if rule["matchListing"]?.bool == true, let input, let listed = world.disc { stdout.append(Self.listedProbe(listed, Self.lastPart(input))) }

        let exit: Int? = switch rule["exit"] {
        case nil: 0
        case .string("never")?: nil
        case let e?: Int(e.int ?? 0)
        }
        let ignore = rule["ignore"]?.array?.compactMap(\.string) ?? (tool == "makemkvcon" ? ["SIGINT"] : nil)
        return Run(stdout: stdout, stderr: stderr, exit: exit, silence: rule["silence"]?.double, ignore: ignore)
    }

    /// The disc a makemkvcon source reads: a backup the tools made holds the disc it was made from; otherwise the world's.
    private func discOf(_ source: String) -> JsonValue? {
        if source.hasPrefix("file:") || source.hasPrefix("iso:"), let colon = source.firstIndex(of: ":") {
            let path = String(source[source.index(after: colon)...])
            let made = lock.withLock { backups }
            if let found = made.first(where: { path.utf8.starts(with: ($0.folder + "/").utf8) }) { return found.disc }
        }
        return world.discOf(source)
    }

    private static func option(_ a: [String], _ name: String) -> String? {
        guard let i = a.firstIndex(of: name), i + 1 < a.count else { return nil }
        return a[i + 1]
    }

    private static func lastPart(_ path: String) -> String {
        guard let i = path.utf8.lastIndex(where: { $0 == UInt8(ascii: "/") || $0 == UInt8(ascii: "\\") }) else { return path }
        return String(decoding: path.utf8[path.utf8.index(after: i)...], as: UTF8.self)
    }

    private static func matches(_ when: JsonValue?, _ command: String, _ mk: Args?, _ a: [String], _ input: String?) -> Bool {
        guard let when else { return true }
        if let c = when["command"]?.string, c != command { return false }
        if let t = when["title"]?.string, mk?.title != t { return false }
        if let include = when["argsInclude"]?.array, include.contains(where: { !a.contains($0.string ?? "") }) { return false }
        if let file = when["file"]?.string, input == nil || !glob(file, input!) { return false }
        return true
    }

    /// A glob over a whole string: * any run of characters, ? one.
    static func glob(_ pattern: String, _ text: String) -> Bool {
        let p = Array(pattern.unicodeScalars), t = Array(text.unicodeScalars)
        func match(_ i: Int, _ j: Int) -> Bool {
            if i == p.count { return j == t.count }
            if p[i] == "*" { return (j...t.count).contains { match(i + 1, $0) } }
            guard j < t.count, p[i] == "?" || p[i] == t[j] else { return false }
            return match(i + 1, j + 1)
        }
        return match(0, 0)
    }

    /// mkvmerge -J's answer, in the shape MkvProbe reads.
    private static func probeJson(_ seconds: Double, _ tracks: [String], _ chapters: Int) -> String {
        let value = JsonValue.object([
            ("container", .object([("properties", .object([("duration", .integer(Int64((seconds * 1e9).rounded())))]))])),
            ("tracks", .array(tracks.enumerated().map { .object([("id", .integer(Int64($0.offset))), ("type", .string($0.element))]) })),
            ("chapters", .array(chapters > 0 ? [.object([("num_entries", .integer(Int64(chapters)))])] : [])),
        ])
        var text = value.description
        while text.hasSuffix("\n") { text.removeLast() }
        return text
    }

    /// What the listing says of the title a file came from (title_t<i>.mkv, else the first): its duration, a video and an
    /// audio track, its chapters.
    private static func listedProbe(_ disc: JsonValue, _ file: String) -> String {
        let titles = disc["titles"]?.array ?? []
        var index = 0
        if let r = file.range(of: "title_t") {
            let digits = file[r.upperBound...].prefix { $0.isASCII && $0.isNumber }
            if let n = Int(digits) { index = n }
        }
        let title = index < titles.count ? titles[index] : .object([])
        return probeJson(Double(ScenarioWorld.seconds(title["duration"]?.string ?? "0")), ["video", "audio"], Int(title["chapters"]?.int ?? 2))
    }

    private func write(_ path: String, _ bytes: [UInt8]) {
        if fs.frozen { return }
        fs.put(path, bytes)
    }

    /// Appends a line to a transcript (not once the files are frozen: a killed engine's).
    fileprivate func append(_ transcript: String?, _ line: String) {
        guard let path = transcript, !path.isEmpty, !fs.frozen else { return }
        let before = fs.exists(path) ? (try? fs.read(path)) ?? [] : []
        fs.put(path, before + Array((line + "\n").utf8))
    }

    /// `exit status 0`, `signal 15` or `abandoned`, then `, stalled` and `, cancelled`, as the platform launchers end a
    /// transcript.
    fileprivate static func ending(_ exit: ProcessExit) -> String {
        var s = exit.abandoned ? "abandoned" : exit.signal.map { "signal \($0)" } ?? "exit status \(exit.status)"
        if exit.stalled != nil { s += ", stalled" }
        if exit.cancelled { s += ", cancelled" }
        return s
    }

    fileprivate static func signalNumber(_ signal: String) -> Int {
        switch signal {
        case "SIGINT": 2
        case "SIGKILL": 9
        default: 15
        }
    }

    // MARK: A process

    fileprivate final class Process: RunningProcess, @unchecked Sendable {
        private let tools: ScenarioTools
        private let spec: ProcessSpec
        private let run: Run
        private let lock = NSLock()
        private var buffer: [OutputLine] = []
        private var reader: CheckedContinuation<OutputLine?, Never>?
        private var complete = false
        private var readCount = 0
        private var writtenCount = 0
        private var result: ProcessExit?
        private var waiters: [CheckedContinuation<ProcessExit, Never>] = []
        private var timers: [any TimerHandle] = []
        private var reason: StopReason?
        private var stalled: Duration?
        private var isEnded = false

        init(tools: ScenarioTools, spec: ProcessSpec, run: Run) {
            self.tools = tools
            self.spec = spec
            self.run = run
        }

        /// Its lines are all read (it may still be running).
        var drained: Bool { lock.withLock { readCount == writtenCount } }

        func begin() {
            let command = ([spec.executable] + spec.arguments).map(ArgumentSplitter.quote).joined(separator: " ")
            tools.append(spec.transcript, "==== \(Instant.format(tools.clock.now())) $ \(command)")
            let at = tools.clock.now()
            for line in run.stdout { emit(OutputLine(stream: .stdout, text: line, at: at)) }
            for line in run.stderr { emit(OutputLine(stream: .stderr, text: line, at: at)) }
            if let silence = run.silence, silence > 0 {
                let t = after(Int64((silence * 1000).rounded())) { [self] in natural() }
                lock.withLock { timers.append(t) }
                watch()
            } else if run.exit == nil {
                watch()
            } else {
                natural()
            }
        }

        private func emit(_ line: OutputLine) {
            lock.lock()
            writtenCount += 1
            if let r = reader {
                reader = nil
                readCount += 1
                lock.unlock()
                r.resume(returning: line)
            } else {
                buffer.append(line)
                lock.unlock()
            }
            tools.append(spec.transcript, line.text)
        }

        private func after(_ ms: Int64, _ then: @escaping @Sendable () -> Void) -> any TimerHandle {
            tools.clock.timer(.at(instant: Instant(unixMilliseconds: tools.clock.now().unixMilliseconds + ms)), handler: then)
        }

        /// The stall watchdog: it is quiet from now on, so it is stopped as stalled once the timeout has passed.
        private func watch() {
            guard let limit = spec.stallTimeout else { return }
            let t = after(Int64((limit.seconds * 1000).rounded())) { [self] in
                let go = lock.withLock { () -> Bool in
                    if isEnded || reason != nil { return false }
                    stalled = limit
                    return true
                }
                if go { stop(.stalled) }
            }
            lock.withLock { timers.append(t) }
        }

        /// The end its script gives it, unless it was stopped meanwhile (exit: never stays running).
        private func natural() {
            if lock.withLock({ reason != nil }) { return }
            if let status = run.exit { end(ProcessExit(status: status)) }
        }

        func lines() -> AsyncStream<OutputLine> {
            AsyncStream(unfolding: { [self] in await next() })
        }

        private func next() async -> OutputLine? {
            await withCheckedContinuation { (c: CheckedContinuation<OutputLine?, Never>) in
                lock.lock()
                if !buffer.isEmpty {
                    let line = buffer.removeFirst()
                    readCount += 1
                    lock.unlock()
                    c.resume(returning: line)
                } else if complete {
                    lock.unlock()
                    c.resume(returning: nil)
                } else {
                    reader = c
                    lock.unlock()
                }
            }
        }

        func wait() async -> ProcessExit {
            await withCheckedContinuation { (c: CheckedContinuation<ProcessExit, Never>) in
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

        func transcriptProblem() -> BroMessage? { nil }

        func stop(_ why: StopReason) {
            let go = lock.withLock { () -> Bool in
                if isEnded || reason != nil { return false }
                reason = why
                return true
            }
            if !go { return }
            signal(spec.stopPolicy == .interruptFirst ? ["SIGINT", "SIGTERM", "SIGKILL"] : ["SIGTERM", "SIGKILL"], 0)
        }

        /// Sends signals[i]: one it doesn't ignore ends it; otherwise the next comes 5 s later, and 30 s after an ignored
        /// SIGKILL it is abandoned.
        private func signal(_ signals: [String], _ i: Int) {
            if lock.withLock({ isEnded }) { return }
            let signal = signals[i]
            if run.ignore?.contains(signal) != true {
                end(ProcessExit(status: -1, signal: ScenarioTools.signalNumber(signal), stalled: lock.withLock { stalled }, cancelled: cancelled()))
                return
            }
            let t = i + 1 < signals.count
                ? after(ScenarioTools.graceMs) { [self] in self.signal(signals, i + 1) }
                : after(ScenarioTools.abandonMs) { [self] in
                    end(ProcessExit(status: -1, signal: nil, stalled: lock.withLock { stalled }, abandoned: true, cancelled: cancelled()))
                }
            lock.withLock { timers.append(t) }
        }

        private func cancelled() -> Bool { lock.withLock { reason == .cancelled || reason == .shutdown } }

        /// Marks it ended (once): its timers cancelled; false when it had ended.
        private func markEnded() -> Bool {
            let pending = lock.withLock { () -> [any TimerHandle]? in
                if isEnded { return nil }
                isEnded = true
                let all = timers
                timers = []
                return all
            }
            guard let pending else { return false }
            for t in pending { t.cancel() }
            return true
        }

        private func finish(_ exit: ProcessExit) {
            lock.lock()
            complete = true
            let r = reader
            reader = nil
            result = exit
            let resume = waiters
            waiters = []
            lock.unlock()
            r?.resume(returning: nil)
            for c in resume { c.resume(returning: exit) }
        }

        private func end(_ exit: ProcessExit) {
            guard markEnded() else { return }
            tools.append(spec.transcript, "==== \(Instant.format(tools.clock.now())) \(ScenarioTools.ending(exit))")
            tools.ended(self)
            finish(exit)
        }

        /// The engine was killed: it ends at once, without another line.
        func killed() {
            guard markEnded() else { return }
            finish(ProcessExit(status: -1, signal: 9))
        }
    }
}
