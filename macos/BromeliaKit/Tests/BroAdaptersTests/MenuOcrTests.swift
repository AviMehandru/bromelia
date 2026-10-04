import BroAdapters
import BroDomain
import BroFoundation
import BroPorts
import BroTestSupport
import Foundation
import Testing

/// shared/fixtures/adapters/menu-ocr.cases.json.
@Suite(.serialized) struct MenuOcrTests {
    let dir = (NSTemporaryDirectory() as NSString).appendingPathComponent("bromelia-ocr-\(UUID().uuidString)")

    func r(_ s: String) -> String { s.replacingOccurrences(of: "<dir>", with: dir) }

    @Test func theSharedCasesPass() async throws {
        let doc = try Fixtures.json("adapters/menu-ocr.cases.json")
        var failures: [String] = []
        defer { try? FileManager.default.removeItem(atPath: dir) }
        for c in doc["cases"]?.array ?? [] {
            let given = c["given"]!, expect = c["expect"]!
            do {
                try? FileManager.default.removeItem(atPath: dir)
                try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
                let launcher = ScriptedProcessLauncher()
                launcher.lines = (given["lines"]?.array ?? []).map { $0.string! }
                launcher.exitCode = Int(given["exitCode"]?.int ?? 0)
                if let w = given["writeArgument"] { launcher.writeArgument = (Int(w["index"]!.int!), w["text"]!.string!) }
                var tools: [ToolKind: String] = [:]
                for t in given["located"]?.array ?? [] { tools[ToolKind(rawValue: t.string!)!] = "/opt/" + t.string! }
                let ocr = MenuOcr(launcher: launcher, locator: MapLocator(paths: tools), fs: PlatformFileSystem())
                let cancel = CancellationToken()
                if given["cancelled"]?.bool == true { cancel.cancel() }
                do {
                    if given["op"]?.string == "extractStills" {
                        let source = VideoTsByteSource.open(Fixtures.url("adapters/video-ts.iso").path)!
                        let cells = given["cells"]!.array!.map { CellRef(file: $0.array![0].string!, firstSector: $0.array![1].int!, endSector: $0.array![2].int!) }
                        let stills = try await ocr.extractStills(source, cells: cells, dir: dir, cancel: cancel)
                        try Fixtures.check(expect["error"] == nil, "expected \(expect["error"]!)")
                        try Fixtures.same(expect["stills"]!.array!.map { r($0.string!) }, stills, "stills")
                    } else {
                        let numbers = try await ocr.readNumbers(given["stills"]!.array!.map { r($0.string!) }, cancel: cancel)
                        try Fixtures.check(expect["error"] == nil, "expected \(expect["error"]!)")
                        try Fixtures.same(expect["numbers"]!.array!.map { Int($0.int!) }, numbers, "numbers")
                    }
                } catch let error as BroError {
                    try Fixtures.same(expect["error"]?["code"]?.string, error.code, "error")
                    for (k, v) in expect["error"]?["params"]?.members ?? [] { try Fixtures.same(v, JsonValue.object(error.params)[k] ?? .null, k) }
                }
                if let started = expect["started"]?.int { try Fixtures.same(Int(started), launcher.started.count, "started") }
                if let argv = expect["argv"]?.array {
                    try Fixtures.same(argv.map { $0.array!.map { r($0.string!) } }, launcher.started.map { [$0.executable] + $0.arguments }, "argv")
                }
                if let stall = expect["stallSeconds"]?.double {
                    try Fixtures.check(launcher.started.allSatisfy { $0.stallTimeout?.seconds == stall }, "stall")
                }
                if let left = expect["left"]?.array {
                    let names = try FileManager.default.contentsOfDirectory(atPath: dir).sorted()
                    try Fixtures.same(left.map { $0.string! }, names, "left")
                }
            } catch {
                failures.append("\(c["id"]!.string!): \(error)")
            }
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }
}
