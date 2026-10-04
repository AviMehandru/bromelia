import BroAdapters
import BroDomain
import BroFoundation
import BroPorts
import BroTestSupport
import Foundation
import Testing

/// shared/fixtures/adapters/settings-isolation.cases.json.
struct HomeDirIsolationTests {
    let root = URL(fileURLWithPath: NSTemporaryDirectory()).resolvingSymlinksInPath().path + "/bromelia-home-" + UUID().uuidString

    func p(_ relative: String) -> String { root + "/" + relative }

    func shown(_ path: String?) -> String { path.map { "<root>/" + $0.dropFirst(root.count + 1) } ?? "(none)" }

    @Test func theSharedCasesPass() throws {
        defer { try? FileManager.default.removeItem(atPath: root) }
        let failures = try Fixtures.runCases("adapters/settings-isolation.cases.json") { _, given, expect in
            try? FileManager.default.removeItem(atPath: root)
            try FileManager.default.createDirectory(atPath: root, withIntermediateDirectories: true)
            for (name, text) in given["before"]?.members ?? [] {
                try FileManager.default.createDirectory(atPath: (p(name) as NSString).deletingLastPathComponent, withIntermediateDirectories: true)
                try Data((text.string ?? "").utf8).write(to: URL(fileURLWithPath: p(name)))
            }
            var settings: [String: String] = [:]
            for (k, v) in given["settings"]?.members ?? [] { settings[k] = v.string }
            let work = p(given["workDirectory"]!.string!)
            let run = MakemkvRunSettings(settings: settings, profileXml: given["profileXml"]?.string, dataDir: "/unused", workDirectory: work)
            let lease = try HomeDirIsolation(fs: PlatformFileSystem(), layout: HomeLayout(rawValue: given["layout"]!.string!)!).prepare(run)
            lease.firstOutput()
            lease.release()

            for (k, v) in expect["environment"]?.members ?? [] { try Fixtures.same(v.string!, shown(lease.environment()[k]), k) }
            try Fixtures.same(expect["profilePath"]?.string ?? "(none)", shown(lease.profilePath()), "profilePath")
            var files: [String] = []
            let e = FileManager.default.enumerator(atPath: work)!
            while let name = e.nextObject() as? String {
                var isDir: ObjCBool = false
                if FileManager.default.fileExists(atPath: work + "/" + name, isDirectory: &isDir), !isDir.boolValue {
                    files.append(String((work + "/" + name).dropFirst(root.count + 1)))
                }
            }
            let want = (expect["files"]?.members ?? []).map(\.key)
            try Fixtures.same(want.sorted(), files.sorted(), "files")
            for (name, text) in expect["files"]?.members ?? [] {
                try Fixtures.same(text.string!, try String(contentsOfFile: p(name), encoding: .utf8), name)
            }
            for (name, mode) in expect["modes"]?.members ?? [] {
                let actual = (try FileManager.default.attributesOfItem(atPath: p(name))[.posixPermissions] as? NSNumber)?.int64Value
                try Fixtures.same(mode.int, actual, "mode of \(name)")
            }
            return true
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }
}
