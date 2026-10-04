import BroAdapters
import BroDomain
import BroFoundation
import BroPorts
import BroTestSupport
import Foundation
import Testing

/// A clock whose timers fire as soon as they are set (when asked to), recording when they were for.
final class FiringClock: Clock, @unchecked Sendable {
    let at = Instant(unixMilliseconds: 1_790_000_000_000)
    private let lock = NSLock()
    private var scheduled: [TimerSchedule] = []
    var fire = false
    var timers: [TimerSchedule] { lock.withLock { scheduled } }

    func now() -> Instant { at }
    func monotonic() -> Duration { Duration(seconds: 0) }
    func sleep(_ duration: Duration, cancel: CancellationToken) async throws(BroError) {}

    func timer(_ schedule: TimerSchedule, handler: @escaping @Sendable () -> Void) -> any TimerHandle {
        lock.withLock { scheduled.append(schedule) }
        if fire { handler() }
        return Handle()
    }

    private final class Handle: TimerHandle {
        func cancel() {}
    }
}

/// Keeps every event.
final class RecordingSink: RunSink, @unchecked Sendable {
    private let lock = NSLock()
    private var all: [RobotEvent] = []
    var events: [RobotEvent] { lock.withLock { all } }
    func event(_ event: RobotEvent) { lock.withLock { all.append(event) } }
}

/// shared/fixtures/adapters/handbrake.cases.json.
@Suite(.serialized) struct HandBrakeTests {
    let root = (NSTemporaryDirectory() as NSString).appendingPathComponent("bromelia-hb-\(UUID().uuidString)")
    let home = (NSTemporaryDirectory() as NSString).appendingPathComponent("bromelia-home")

    func r(_ s: String) -> String {
        s.replacingOccurrences(of: "<root>", with: root).replacingOccurrences(of: "<home>", with: home)
            .replacingOccurrences(of: "<handbrake>", with: "/opt/HandBrakeCLI")
    }

    @Test func theSharedCasesPass() async throws {
        let doc = try Fixtures.json("adapters/handbrake.cases.json")
        var failures: [String] = []
        defer { try? FileManager.default.removeItem(atPath: root) }
        for c in doc["cases"]?.array ?? [] {
            let given = c["given"]!, expect = c["expect"]!
            do {
                try? FileManager.default.removeItem(atPath: root)
                try FileManager.default.createDirectory(atPath: root, withIntermediateDirectories: true)
                for f in given["files"]?.array ?? [] {
                    let path = r(f.string!)
                    try FileManager.default.createDirectory(atPath: (path as NSString).deletingLastPathComponent, withIntermediateDirectories: true)
                    FileManager.default.createFile(atPath: path, contents: Data())
                }
                var lines: [String] = [], stderr: Set<Int> = []
                if let file = given["linesFile"]?.string {
                    for l in String(decoding: try Fixtures.bytes(file), as: UTF8.self).split(separator: "\n") where !l.isEmpty {
                        stderr.insert(lines.count)
                        lines.append(String(l))
                    }
                }
                for l in given["lines"]?.array ?? [] {
                    if let e = l["stderr"]?.string {
                        stderr.insert(lines.count)
                        lines.append(e)
                    } else {
                        lines.append(l.string!)
                    }
                }
                let launcher = ScriptedProcessLauncher()
                launcher.lines = lines
                launcher.stderr = stderr
                launcher.exitCode = Int(given["exitCode"]?.int ?? 0)
                let tools: [ToolKind: String] = given["located"]?.bool == false ? [:] : [.handbrake: "/opt/HandBrakeCLI"]
                let clock = FiringClock()
                clock.fire = given["timerFires"]?.bool == true
                let handbrake = HandBrake(launcher: launcher, locator: MapLocator(paths: tools), clock: clock, home: home)
                let cancel = CancellationToken()
                if given["cancelled"]?.bool == true { cancel.cancel() }
                let sink = RecordingSink()
                do {
                    if given["op"]?.string == "presets" {
                        let presets = try await handbrake.presets(cancel)
                        if let all = expect["presets"]?.array { try Fixtures.same(all.map { $0.string! }, presets, "presets") }
                        if let n = expect["count"]?.int { try Fixtures.same(Int(n), presets.count, "count") }
                        if let first = expect["first"]?.string { try Fixtures.same(first, presets.first, "first") }
                        if let last = expect["last"]?.string { try Fixtures.same(last, presets.last, "last") }
                        for p in expect["includes"]?.array ?? [] { try Fixtures.check(presets.contains(p.string!), "includes \(p)") }
                    } else {
                        let stepText = r(String(decoding: JsonValue.encodeCanonical(given["step"]!), as: UTF8.self))
                        let step = StepDefinition.decode(JsonValue.parse(Array(stepText.utf8))!)
                        let run = try await handbrake.encode(step, input: r(given["input"]!.string!), output: r(given["output"]!.string!), sink: sink,
                                                             cancel: cancel)
                        try Fixtures.check(expect["error"] == nil, "expected \(expect["error"]!)")
                        try Fixtures.same(expect["status"]?.int, Int64(run.exit.status), "status")
                        try Fixtures.same(expect["timedOut"]?.bool, run.timedOut, "timedOut")
                    }
                } catch let error as BroError {
                    try Fixtures.same(expect["error"]?["code"]?.string, error.code, "error")
                    for (k, v) in expect["error"]?["params"]?.members ?? [] {
                        try Fixtures.same(r(v.string!), JsonValue.object(error.params)[k]?.string, k)
                    }
                }
                if expect["started"]?.bool == false { try Fixtures.same(0, launcher.started.count, "started") }
                if let argv = expect["argv"]?.array {
                    let spec = launcher.started.first
                    try Fixtures.same(argv.map { r($0.string!) }, spec.map { [$0.executable] + $0.arguments }, "argv")
                }
                if let wd = expect["workingDirectory"]?.string { try Fixtures.same(r(wd), launcher.started.first?.workingDirectory, "workingDirectory") }
                let progress = sink.events.compactMap { e -> String? in
                    if case let .progressValue(current, total, max) = e { return "\(current)/\(total)/\(max)" }
                    return nil
                }
                if let want = expect["progress"]?.array {
                    try Fixtures.same(want.map { "\($0.array![0].int!)/\($0.array![1].int!)/10000" }, progress, "progress")
                }
                if let want = expect["raw"]?.array {
                    let raw = sink.events.compactMap { e -> String? in
                        if case let .raw(text) = e { return text }
                        return nil
                    }
                    try Fixtures.same(want.map { $0.string! }, raw, "raw")
                }
                if let seconds = expect["timer"]?.int {
                    try Fixtures.same([TimerSchedule.at(instant: Instant(unixMilliseconds: clock.at.unixMilliseconds + seconds * 1000))], clock.timers, "timer")
                } else {
                    try Fixtures.same(0, clock.timers.count, "timers")
                }
            } catch {
                failures.append("\(c["id"]!.string!): \(error)")
            }
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }
}
