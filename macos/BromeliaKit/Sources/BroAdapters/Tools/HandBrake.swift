import BroDomain
import BroFoundation
import BroPorts
import Foundation

/// HandBrakeCLI (plan §10.1; shared/fixtures/adapters/handbrake.cases.json): one encode of a ripped MKV with the
/// step's preset, next to the archive (never over it: the step picks the output), and the presets to offer. The step's
/// own executable is used when it sets one; otherwise the locator's.
public final class HandBrake: Sendable {
    /// Presets built into HandBrake 1.6 and later (any preset name works): offered when HandBrakeCLI can't list its own.
    public static let builtInPresets = ["H.265 MKV 1080p30", "H.265 MKV 2160p60 4K", "H.264 MKV 1080p30", "H.264 MKV 480p30", "Fast 1080p30",
                                        "HQ 1080p30 Surround", "Super HQ 1080p30 Surround", "Fast 2160p60 4K HEVC"]

    private static let max = 10000
    private let launcher: any ProcessLauncher
    private let locator: any ToolLocator
    private let clock: any Clock
    private let home: String

    /// - Parameter home: the home folder a leading ~ stands for (the step's executable and preset file).
    public init(launcher: any ProcessLauncher, locator: any ToolLocator, clock: any Clock, home: String) {
        self.launcher = launcher
        self.locator = locator
        self.clock = clock
        self.home = home
    }

    /// HandBrakeCLI --preset-list's names, in its order; `builtInPresets` when HandBrakeCLI is missing or fails.
    public func presets(_ cancel: CancellationToken) async throws(BroError) -> [String] {
        guard let exe = locator.locate(.handbrake).path else { return Self.builtInPresets }
        if cancel.isCancelled { throw BroError(MessageCode.jobCancelled.rawValue) }
        var names: [String] = []
        var inCategory = false
        let exit = try await ToolRun.run(launcher, ProcessSpec(executable: exe, arguments: ["--preset-list"], environment: [:], stopPolicy: .interruptFirst,
                                                               stallTimeout: Duration(seconds: 60)), cancel: cancel) { line in
            let text = line.text
            if !text.hasPrefix(" ") {
                inCategory = text.hasSuffix("/") // a category, or anything else at column 0
            } else if inCategory, text.hasPrefix("    "), text.count > 4, text.dropFirst(4).first != " " {
                names.append(String(text.dropFirst(4)).trimmingCharacters(in: .whitespaces))
            }
            return nil
        }
        if exit.cancelled { throw BroError(MessageCode.jobCancelled.rawValue) }
        return exit.status == 0 && !names.isEmpty ? names : Self.builtInPresets
    }

    /// Encodes `input` to `output` (whose folder must exist) in that folder. Progress goes to the sink as progressValue
    /// (of 10000), the lines HandBrakeArgs.keepLine keeps as raw.
    public func encode(_ step: StepDefinition, input: String, output: String, sink: any RunSink, cancel: CancellationToken) async throws(BroError)
        -> HandBrakeRun
    {
        let exe = try executable(step)
        if cancel.isCancelled { throw BroError(MessageCode.jobCancelled.rawValue) }
        let timedOut = Flag()
        var timer: (any TimerHandle)?
        defer { timer?.cancel() }
        var filter = ProgressFilter()
        let spec = ProcessSpec(executable: exe, arguments: HandBrakeArgs.build(step, input: input, output: output, home: home), environment: [:],
                               workingDirectory: (output as NSString).deletingLastPathComponent, stopPolicy: .interruptFirst)
        let exit = try await ToolRun.run(launcher, spec, cancel: cancel, started: { process in
            guard step.timeoutSeconds > 0 else { return }
            let at = Instant(unixMilliseconds: self.clock.now().unixMilliseconds + Int64(step.timeoutSeconds) * 1000)
            timer = self.clock.timer(.at(instant: at)) {
                timedOut.set()
                process.stop(.timedOut)
            }
        }) { line in
            if let p = Self.progress(line.text) { sink.event(.progressValue(current: p.current, total: p.total, max: Self.max)) }
            if HandBrakeArgs.keepLine(&filter, line: line.text) { sink.event(.raw(text: line.text)) }
            return nil
        }
        if exit.cancelled { throw BroError(MessageCode.jobCancelled.rawValue) }
        return HandBrakeRun(exit: exit, timedOut: timedOut.isSet)
    }

    /// The step's executable (it must exist), or the one the locator finds.
    private func executable(_ step: StepDefinition) throws(BroError) -> String {
        var own = step.handbrake.executable.trimmingCharacters(in: .whitespaces)
        if !own.isEmpty {
            if own == "~" || own.hasPrefix("~/") { own = home.trimmingSuffix("/") + own.dropFirst() }
            var isDirectory: ObjCBool = false
            if FileManager.default.fileExists(atPath: own, isDirectory: &isDirectory), !isDirectory.boolValue { return own }
            throw BroMessage(.toolNotFoundAt, [("tool", .string(ToolKind.handbrake.rawValue)), ("path", .string(own))], severity: .error).toError()
        }
        let info = locator.locate(.handbrake)
        guard let path = info.path else {
            throw (info.why ?? BroMessage(.toolMissing, [("tool", .string(ToolKind.handbrake.rawValue))], severity: .warning)).toError()
        }
        return path
    }

    /// 'Encoding: task t of n, p %' as (this task's share, all tasks' share) of 10000.
    static func progress(_ line: String) -> (current: Int, total: Int)? {
        guard let start = line.range(of: "Encoding: task ") else { return nil }
        let scanner = Scanner(string: String(line[start.upperBound...]))
        scanner.charactersToBeSkipped = nil
        guard let task = scanner.scanInt(), scanner.scanString(" of ") != nil, let tasks = scanner.scanInt(), scanner.scanString(", ") != nil,
              let whole = scanner.scanInt(), scanner.scanString(".") != nil,
              let digits = scanner.scanCharacters(from: .decimalDigits), digits.count >= 2, let hundredths = Int(digits.prefix(2))
        else { return nil }
        let n = Swift.max(1, tasks)
        let current = Swift.min(Self.max, whole * 100 + hundredths)
        let total = (Swift.min(Swift.max(task - 1, 0), n - 1) * Self.max + current) / n
        return (current, total)
    }

    /// Set once, from the timer's thread.
    private final class Flag: @unchecked Sendable {
        private let lock = NSLock()
        private var value = false
        func set() { lock.withLock { value = true } }
        var isSet: Bool { lock.withLock { value } }
    }
}

private extension String {
    func trimmingSuffix(_ suffix: Character) -> String {
        var s = self
        while s.count > 1, s.last == suffix { s.removeLast() }
        return s
    }
}
