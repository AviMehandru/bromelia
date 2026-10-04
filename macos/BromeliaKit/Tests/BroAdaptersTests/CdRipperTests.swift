import BroAdapters
import BroDomain
import BroFoundation
import BroPorts
import BroTestSupport
import Foundation
import Testing

/// shared/fixtures/adapters/cd-ripper.cases.json.
@Suite(.serialized) struct CdRipperTests {
    let root = (NSTemporaryDirectory() as NSString).appendingPathComponent("bromelia-cd-\(UUID().uuidString)")

    func r(_ s: String) -> String { s.replacingOccurrences(of: "<root>", with: root) }

    @Test func theSharedCasesPass() async throws {
        let doc = try Fixtures.json("adapters/cd-ripper.cases.json")
        var failures: [String] = []
        defer { try? FileManager.default.removeItem(atPath: root) }
        for c in doc["cases"]?.array ?? [] {
            let given = c["given"]!, expect = c["expect"]!
            do {
                try? FileManager.default.removeItem(atPath: root)
                let dest = r(given["dest"]!.string!)
                try FileManager.default.createDirectory(atPath: dest, withIntermediateDirectories: true)
                var lines: [String] = [], stderr: Set<Int> = []
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
                launcher.stalls = given["stalls"]?.bool == true
                if let saves = given["saves"]?.string {
                    launcher.writeFileIn = dest
                    launcher.writeFileName = saves
                }
                var tools: [ToolKind: String] = [:]
                for t in given["located"]?.array ?? [] { tools[ToolKind(rawValue: t.string!)!] = "/opt/" + t.string! }
                let ripper = CdRipper(launcher: launcher, locator: MapLocator(paths: tools), fs: PlatformFileSystem())
                let cancel = CancellationToken()
                if given["cancelled"]?.bool == true { cancel.cancel() }
                let sink = RecordingSink()
                do {
                    let files = try await ripper.rip(given["device"]!.string!, dest: dest, command: given["command"]?.string,
                                                     stallMinutes: Int(given["stallMinutes"]!.int!), sink: sink, cancel: cancel)
                    try Fixtures.check(expect["error"] == nil, "expected \(expect["error"]!)")
                    try Fixtures.same(expect["files"]!.array!.map { r($0.string!) }, files, "files")
                } catch let error as BroError {
                    try Fixtures.same(expect["error"]?["code"]?.string, error.code, "error")
                    for (k, v) in expect["error"]?["params"]?.members ?? [] { try Fixtures.same(v, JsonValue.object(error.params)[k] ?? .null, k) }
                }
                if expect["started"]?.bool == false { try Fixtures.same(0, launcher.started.count, "started") }
                if let argv = expect["argv"]?.array {
                    try Fixtures.same(argv.map { $0.string! }, launcher.started.first.map { [$0.executable] + $0.arguments }, "argv")
                }
                if let wd = expect["workingDirectory"]?.string { try Fixtures.same(r(wd), launcher.started.first?.workingDirectory, "workingDirectory") }
                if let stall = expect["stallSeconds"] { try Fixtures.same(stall.double, launcher.started.first?.stallTimeout?.seconds, "stall") }
                if let raw = expect["raw"]?.array {
                    let got = sink.events.compactMap { e -> String? in
                        if case let .raw(text) = e { return text }
                        return nil
                    }
                    try Fixtures.same(raw.map { $0.string! }, got, "raw")
                }
            } catch {
                failures.append("\(c["id"]!.string!): \(error)")
            }
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }
}
