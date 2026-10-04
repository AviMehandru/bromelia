import BroAdapters
import BroDomain
import BroFoundation
import BroPorts
import BroTestSupport
import Foundation
import Testing

/// shared/fixtures/adapters/mkvtoolnix.cases.json.
struct MkvToolNixTests {
    let root = URL(fileURLWithPath: NSTemporaryDirectory()).resolvingSymlinksInPath().path + "/bromelia-mkv-" + UUID().uuidString

    func r(_ s: String) -> String { s.replacingOccurrences(of: "<root>", with: root) }
    func shown(_ s: String) -> String { s.replacingOccurrences(of: root, with: "<root>") }

    @Test func theSharedCasesPass() async throws {
        defer { try? FileManager.default.removeItem(atPath: root) }
        let doc = try Fixtures.json("adapters/mkvtoolnix.cases.json")
        var failures: [String] = []
        for c in doc["cases"]?.array ?? [] {
            do { try await run(c["given"]!, c["expect"]!) } catch { failures.append("\(c["id"]!.string!): \(error)") }
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    func run(_ given: JsonValue, _ expect: JsonValue) async throws {
        try? FileManager.default.removeItem(atPath: root)
        try FileManager.default.createDirectory(atPath: root, withIntermediateDirectories: true)
        let launcher = ScriptedProcessLauncher()
        launcher.lines = (given["lines"]?.array ?? []).map { $0.string! }
        launcher.exitCode = Int(given["exitCode"]?.int ?? 0)
        launcher.splitParts = Int(given["splitParts"]?.int ?? 0)
        if let w = given["writeArgument"] { launcher.writeArgument = (Int(w["index"]!.int!), w["text"]!.string!) }
        var tools: [ToolKind: String] = [.mkvmerge: "/opt/mkvmerge"]
        if given["mkvextract"]?.isNull != true { tools[.mkvextract] = "/opt/mkvextract" }
        let mkv = MkvToolNix(launcher: launcher, fs: PlatformFileSystem(), locator: MapLocator(paths: tools), workDirectory: root)
        let call = given["call"]!.array!
        let cancel = CancellationToken()
        do {
            switch call[0].string! {
            case "probe":
                let probe = try await mkv.probe(r(call[1].string!), cancel: cancel)
                if expect["probe"]?.isNull == true { try Fixtures.check(probe == nil, "no probe") } else {
                    let p = try #require(probe)
                    if let d = expect["probe"]?["durationSeconds"]?.double { try Fixtures.same(d, p.durationSeconds, "duration") }
                    if let t = expect["probe"]?["tracks"]?.int { try Fixtures.same(Int(t), p.tracks.count, "tracks") }
                    if let c = expect["probe"]?["chapterCount"]?.int { try Fixtures.same(Int(c), p.chapterCount, "chapters") }
                }
            case "remux":
                try Fixtures.same(expect["ok"]?.bool, try await mkv.remux((call[1].array ?? []).map { r($0.string!) }, cancel: cancel), "ok")
            case "split":
                let parts = try await mkv.split((call[1].array ?? []).map { Int($0.int!) }, input: r(call[2].string!), cancel: cancel)
                if expect["parts"]?.isNull == true { try Fixtures.check(parts == nil, "no parts") } else { try Fixtures.same(Int(expect["parts"]!.int!), parts?.count, "parts") }
                if let left = expect["leftOver"]?.int {
                    let n = try FileManager.default.contentsOfDirectory(atPath: root).filter { $0.hasPrefix(".bromelia-split-") }.count
                    try Fixtures.same(Int(left), n, "left over")
                }
            case "chapterTimes":
                let times = try await mkv.chapterTimes(r(call[1].string!), cancel: cancel)
                try Fixtures.same((expect["times"]?.array ?? []).map { $0.double! }, times?.map(\.seconds), "times")
                try Fixtures.check(try FileManager.default.contentsOfDirectory(atPath: root).filter { $0.hasPrefix(".bromelia-chapters-") }.isEmpty, "temp removed")
            default: throw FixtureError("no test handles this call")
            }
            try Fixtures.check(expect["error"] == nil, "expected \(expect["error"]!)")
        } catch let error as BroError {
            try Fixtures.same(expect["error"]?.string, error.code, "error")
        }
        let spec = launcher.started.first
        if expect["launched"]?.bool == false { try Fixtures.check(spec == nil, "not launched") }
        if let exe = expect["executable"]?.string { try Fixtures.same(exe, spec?.executable, "executable") }
        if let args = expect["arguments"]?.array { try Fixtures.same(args.map { $0.string! }, spec?.arguments.map(shown), "arguments") }
        if let start = expect["argumentsStart"]?.array { try Fixtures.same(start.map { $0.string! }, spec.map { Array($0.arguments.prefix(start.count)).map(shown) }, "start") }
        if let end = expect["argumentsEnd"]?.array { try Fixtures.same(end.map { $0.string! }, spec.map { Array($0.arguments.suffix(end.count)).map(shown) }, "end") }
    }
}
