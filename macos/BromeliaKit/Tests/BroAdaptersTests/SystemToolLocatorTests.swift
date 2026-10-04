import BroAdapters
import BroDomain
import BroFoundation
import BroPorts
import BroTestSupport
import Foundation
import Testing

/// shared/fixtures/adapters/tool-locator.cases.json.
struct SystemToolLocatorTests {
    let root = URL(fileURLWithPath: NSTemporaryDirectory()).resolvingSymlinksInPath().path + "/bromelia-tools-" + UUID().uuidString

    func r(_ text: String) -> String { text.replacingOccurrences(of: "<root>", with: root) }

    func lists(_ v: JsonValue?, _ map: (String) -> String) -> [ToolKind: [String]] {
        var out: [ToolKind: [String]] = [:]
        for (k, list) in v?.members ?? [] { out[ToolKind(rawValue: k)!] = (list.array ?? []).map { map($0.string!) } }
        return out
    }

    @Test func theSharedCasesPass() throws {
        defer { try? FileManager.default.removeItem(atPath: root) }
        let failures = try Fixtures.runCases("adapters/tool-locator.cases.json") { _, given, expect in
            try? FileManager.default.removeItem(atPath: root)
            for f in (given["files"]?.array ?? []).map({ $0.string! }) {
                let path = root + "/" + f
                if f.hasSuffix("/") {
                    try FileManager.default.createDirectory(atPath: path, withIntermediateDirectories: true)
                } else {
                    try FileManager.default.createDirectory(atPath: (path as NSString).deletingLastPathComponent, withIntermediateDirectories: true)
                    FileManager.default.createFile(atPath: path, contents: Data())
                }
            }
            var configured: [ToolKind: String] = [:]
            for (k, v) in given["configured"]?.members ?? [] { configured[ToolKind(rawValue: k)!] = r(v.string!) }
            let locator = SystemToolLocator(fs: PlatformFileSystem(), configured: configured, candidates: lists(given["candidates"], r),
                                            names: lists(given["names"], { $0 }), searchPath: (given["searchPath"]?.array ?? []).map { r($0.string!) },
                                            home: r(given["home"]!.string!))
            let info = locator.locate(ToolKind(rawValue: given["tool"]!.string!)!)
            func shown(_ p: String) -> String { "<root>" + p.dropFirst(root.count) }
            if let want = expect["path"]?.string {
                try Fixtures.same(want, info.path.map(shown) ?? "(none)", "path")
                try Fixtures.check(info.why == nil, "no why")
            } else {
                try Fixtures.check(info.path == nil, "not found")
                try Fixtures.same(expect["why"]!["code"]!.string!, info.why?.code.rawValue ?? "", "code")
                for (k, v) in expect["why"]?["params"]?.members ?? [] {
                    let actual = JsonValue.object(info.why!.params)[k]?.string ?? ""
                    try Fixtures.same(v.string!, k == "path" ? shown(actual) : actual, k)
                }
            }
            return true
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    @Test func todaysCandidatesAndNames() {
        let c = PlatformToolPaths.candidates("/Users/me")
        #expect(c[.makemkvcon]?.first == "/Applications/MakeMKV.app/Contents/MacOS/makemkvcon")
        #expect(c[.makemkvcon]?.contains("/Users/me/Applications/MakeMKV.app/Contents/MacOS/makemkvcon") == true)
        #expect(c[.mkvmerge]?.contains("/opt/homebrew/bin/mkvmerge") == true)
        #expect(PlatformToolPaths.names()[.handbrake] == ["HandBrakeCLI"])
        #expect(PlatformToolPaths.names().count == ToolKind.allCases.count)
    }
}
