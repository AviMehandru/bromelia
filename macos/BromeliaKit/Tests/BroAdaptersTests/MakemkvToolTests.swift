import BroAdapters
import BroDomain
import BroFoundation
import BroPorts
import BroTestSupport
import Foundation
import Testing

/// shared/fixtures/adapters/makemkv-tool.cases.json.
struct MakemkvToolTests {
    let root = URL(fileURLWithPath: Fixtures.temporaryDirectory).resolvingSymlinksInPath().path + "/bromelia-makemkv-" + UUID().uuidString

    func shown(_ s: String) -> String { s.replacingOccurrences(of: root + "/home", with: "<work>").replacingOccurrences(of: root, with: "<root>") }

    func source(_ v: JsonValue) -> MakemkvSource {
        if let d = v["drive"] { return .drive(index: Int(d["index"]!.int!), device: d["device"]!.string!) }
        if let i = v["iso"] { return .iso(path: i.string!) }
        return .file(path: v["file"]!.string!)
    }

    func lines(_ given: JsonValue) throws -> [String] {
        if let t = given["transcript"]?.string { return try Fixtures.text(t).split(separator: "\n").map(String.init) }
        return (given["lines"]?.array ?? []).map { $0.string! }
    }

    @Test func theSharedCasesPass() async throws {
        defer { try? FileManager.default.removeItem(atPath: root) }
        let doc = try Fixtures.json("adapters/makemkv-tool.cases.json")
        var failures: [String] = []
        for c in doc["cases"]?.array ?? [] {
            let id = c["id"]!.string!
            do {
                try await run(c["given"]!, c["expect"]!)
            } catch {
                failures.append("\(id): \(error)")
            }
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    func run(_ given: JsonValue, _ expect: JsonValue) async throws {
        try? FileManager.default.removeItem(atPath: root)
        try FileManager.default.createDirectory(atPath: root + "/home", withIntermediateDirectories: true)
        for f in (given["existing"]?.array ?? []).map({ $0.string! }) {
            try FileManager.default.createDirectory(atPath: ((root + "/" + f) as NSString).deletingLastPathComponent, withIntermediateDirectories: true)
            FileManager.default.createFile(atPath: root + "/" + f, contents: Data())
        }
        let destination = given["destination"]?.string.map { root + "/" + $0 }
        if let destination {
            try FileManager.default.createDirectory(atPath: given["destinationExists"]?.bool == false ? (destination as NSString).deletingLastPathComponent : destination,
                                                    withIntermediateDirectories: true)
        }
        let writes = (given["writes"]?.array ?? []).map { $0.string! }
        let cancelSource = CancellationSource()
        let cancel = cancelSource.token
        let launcher = ScriptedProcessLauncher()
        launcher.lines = try lines(given)
        launcher.exitCode = Int(given["exitCode"]?.int ?? 0)
        launcher.writeFileIn = writes.isEmpty ? nil : destination
        launcher.writeFileNames = writes
        launcher.transcriptFails = given["transcriptFails"]?.bool == true
        if let after = given["cancelAfterLines"]?.int { launcher.afterLine = { n in if n == Int(after) { cancelSource.cancel() } } }
        if let gone = given["deleteDestinationAfterLines"]?.int, let destination {
            launcher.afterLine = { n in if n == Int(gone) { try? FileManager.default.removeItem(atPath: destination) } }
        }
        let isolation = RecordingIsolation()
        if let reason = given["releaseProblem"]?.string {
            isolation.releaseProblem = BroMessage(.makemkvKeyNotRemoved, [("path", .string("<work>/.MakeMKV/settings.conf")), ("reason", .string(reason))],
                                                  severity: .warning)
        }
        let makemkvcon: String? = given["makemkvcon"]?.isNull == true ? nil : "/opt/makemkvcon"
        let tool = MakemkvTool(launcher: launcher, fs: PlatformFileSystem(), isolation: isolation, locator: FixedLocator(makemkvcon: makemkvcon))
        let options = MakemkvOptions(minLengthSeconds: given["options"]?["minLengthSeconds"]?.int.map { Int($0) })
        let settings = MakemkvRunSettings(settings: [:], profileXml: given["profileXml"]?.string, dataDir: "/data", workDirectory: root + "/home")
        let invocation = MakemkvInvocation(settings: settings, options: options, stallTimeout: given["stallTimeout"]?.int.map { Duration(seconds: Double($0)) },
                                           transcript: given["transcriptFile"]?.string.map { root + "/" + $0 })
        let sink = CountingSink()
        var run: MakemkvRun?
        var drives: [MakemkvDrive]?
        var listing: Listing?
        do {
            switch given["call"]!.string! {
            case "scanDrives": drives = try await tool.scanDrives(cancel)
            case "listing":
                let l = try await tool.listing(source(given["source"]!), invocation: invocation, sink: sink, cancel: cancel)
                listing = l.listing
                run = l.run
            case "rip": run = try await tool.rip(source(given["source"]!), title: given["title"]!.string!, destination: destination!, invocation: invocation, sink: sink, cancel: cancel)
            case "backup": run = try await tool.backup(source(given["source"]!), decrypt: given["decrypt"]!.bool!, destination: destination!, invocation: invocation, sink: sink, cancel: cancel)
            default: throw FixtureError("no test handles this call")
            }
            try Fixtures.check(expect["error"] == nil, "expected \(expect["error"]!)")
        } catch let e as BroError {
            try Fixtures.same(expect["error"]?.string, e.code, "error")
        }
        let spec = launcher.started.first
        if expect["launched"]?.bool == false { try Fixtures.check(spec == nil, "not launched") }
        if let args = expect["arguments"]?.array { try Fixtures.same(args.map { $0.string! }, spec?.arguments.map(shown), "arguments") }
        if let env = expect["environment"]?.members {
            try Fixtures.same(env.map { "\($0.key)=\($0.value.string!)" }, spec?.environment.sorted { $0.key < $1.key }.map { "\($0.key)=\(shown($0.value))" }, "environment")
        }
        if let wd = expect["workingDirectory"] { try Fixtures.same(wd.string, spec?.workingDirectory.map(shown), "workingDirectory") }
        if let sp = expect["stopPolicy"]?.string { try Fixtures.same(sp, spec?.stopPolicy.rawValue, "stopPolicy") }
        if let stall = expect["stallTimeout"]?.int { try Fixtures.same(Double(stall), spec?.stallTimeout?.seconds, "stallTimeout") }
        if let tf = expect["transcriptFile"]?.string { try Fixtures.same(tf, spec?.transcript.map(shown), "transcriptFile") }
        if let lease = expect["lease"]?.array { try Fixtures.same(lease.map { $0.string! }, isolation.log, "lease") }
        if let stopped = expect["stopped"]?.bool { try Fixtures.same(stopped, launcher.last?.stopReason != nil, "stopped") }
        if let why = expect["stopReason"]?.string { try Fixtures.same(why, launcher.last?.stopReason?.rawValue, "stopReason") }
        if let read = expect["linesRead"]?.int { try Fixtures.same(Int(read), launcher.last?.linesHanded, "linesRead") }
        if let want = expect["drives"]?.array {
            try Fixtures.same(want.map { "\($0.array![0].int!) \($0.array![1].string!) \($0.array![2].string!)" },
                              (drives ?? []).map { "\($0.index) \($0.state.rawValue) \($0.device)" }, "drives")
        }
        if let wl = expect["listing"] {
            try Fixtures.same(wl["volumeName"]?.string, listing?.volumeName, "volumeName")
            try Fixtures.same(wl["titles"]?.int.map { Int($0) }, listing?.titles.count, "titles")
        }
        if let status = expect["status"]?.string { try Fixtures.same(status, run?.outcome.status.rawValue, "status") }
        if let contains = expect["errorContains"]?.string {
            let text = try English.render(run!.outcome.error!.toJson())
            try Fixtures.check(text.contains(contains), "\(text) contains \(contains)")
        }
        if let code = expect["errorCode"]?.string { try Fixtures.same(code, run?.outcome.error?.code.rawValue, "errorCode") }
        if let version = expect["version"]?.string { try Fixtures.same(version, run?.version, "version") }
        if let log = expect["debugLog"]?.string { try Fixtures.same(log, run?.outcome.debugLog, "debugLog") }
        if let produced = expect["produced"]?.array { try Fixtures.same(produced.map { $0.string! }, run?.outcome.produced, "produced") }
        try Fixtures.same(expect["transcriptProblem"]?.string, run?.transcriptProblem?.code.rawValue, "transcriptProblem")
        try Fixtures.same(expect["settingsProblem"]?.string, run?.settingsProblem?.code.rawValue, "settingsProblem")
        if run != nil { try Fixtures.check(sink.events > 0, "the sink heard something") }
    }

    /// A cancelled task ends the lines early: makemkvcon is stopped before it is waited for, not left running with
    /// nobody reading it.
    @Test func aCancelledTaskStopsTheTool() async throws {
        try? FileManager.default.removeItem(atPath: root)
        try FileManager.default.createDirectory(atPath: root + "/home", withIntermediateDirectories: true)
        final class Box: @unchecked Sendable { var task: Task<MakemkvRun?, Never>? }
        let box = Box()
        let launcher = ScriptedProcessLauncher()
        launcher.lines = Array(repeating: "PRGV:1,0,65536", count: 1000)
        launcher.afterLine = { n in if n == 3 { box.task?.cancel() } }
        let tool = MakemkvTool(launcher: launcher, fs: PlatformFileSystem(), isolation: RecordingIsolation(), locator: FixedLocator(makemkvcon: "/opt/makemkvcon"))
        let invocation = MakemkvInvocation(settings: MakemkvRunSettings(settings: [:], dataDir: "/data", workDirectory: root + "/home"), options: MakemkvOptions())
        let task = Task<MakemkvRun?, Never> {
            try? await Task.sleep(nanoseconds: 50_000_000)
            return try? await tool.rip(.drive(index: 0, device: "/dev/rdisk4"), title: "all", destination: root + "/staging", invocation: invocation,
                                       sink: RecordingSink(), cancel: CancellationSource().token)
        }
        box.task = task
        _ = await task.value
        #expect(launcher.last?.stopReason == .cancelled)
        #expect(launcher.last.map { $0.linesHanded < 1000 } == true)
    }
}
