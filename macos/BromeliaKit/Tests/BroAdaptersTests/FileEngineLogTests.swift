import BroAdapters
import BroDomain
import BroFoundation
import BroPorts
import BroTestKit
import BroTestSupport
import Foundation
import Testing

/// shared/fixtures/adapters/engine-log.cases.json.
struct FileEngineLogTests {
    let base = URL(fileURLWithPath: NSTemporaryDirectory()).resolvingSymlinksInPath().path + "/bromelia-engine-log-" + UUID().uuidString

    @Test func theSharedCasesPass() throws {
        defer { try? FileManager.default.removeItem(atPath: base) }
        var n = 0
        let failures = try Fixtures.runCases("adapters/engine-log.cases.json") { _, given, expect in
            n += 1
            let root = base + "/\(n)"
            let logs = root + "/data/logs"
            let path = logs + "/engine.log"
            try FileManager.default.createDirectory(atPath: given["noLogsFolder"]?.bool == true ? root + "/data" : logs, withIntermediateDirectories: true)
            if let before = given["before"]?.string { try Data(before.utf8).write(to: URL(fileURLWithPath: path)) }
            if given["logIsAFolder"]?.bool == true { try FileManager.default.createDirectory(atPath: path, withIntermediateDirectories: false) }
            let clock = BroTestKit.ManualClock()
            let log = FileEngineLog(fs: PlatformFileSystem(), clock: clock, path: path)
            for line in given["lines"]?.array ?? [] {
                clock.advanceNow((line["at"]?.int ?? 0) - clock.now().unixMilliseconds)
                log.log(BroMessage(MessageCode.parse(line["code"]?.string ?? "")!, line["params"]?.members ?? [],
                                   severity: Severity(rawValue: line["severity"]?.string ?? "")!))
            }
            var isDir: ObjCBool = false
            if expect["logIsAFolder"]?.bool == true {
                try Fixtures.check(FileManager.default.fileExists(atPath: path, isDirectory: &isDir) && isDir.boolValue, "engine.log is still a folder")
            }
            if let file = expect["file"]?.string { try Fixtures.same(file, try String(contentsOfFile: path, encoding: .utf8), "engine.log") }
            if let mode = expect["mode"]?.int {
                let perms = (try FileManager.default.attributesOfItem(atPath: path)[.posixPermissions] as? NSNumber)?.intValue ?? -1
                try Fixtures.same(Int(mode), perms, "mode")
            }
            return true
        }
        #expect(failures.isEmpty, "\(failures)")
    }
}
