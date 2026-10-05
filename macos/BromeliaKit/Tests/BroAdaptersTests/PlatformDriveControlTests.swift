import BroAdapters
import BroDomain
import BroFoundation
import BroPorts
import BroTestSupport
import Foundation
import Testing

/// shared/fixtures/adapters/drive-control.cases.json and platform-adapters.contract.json#drutil-match (no eject or tray:
/// real drives are the owner's to test).
struct PlatformDriveControlTests {
    @Test func drutilIsMatchedByProduct() throws {
        let doc = try Fixtures.json("adapters/platform-adapters.contract.json")
        let c = doc["cases"]!.array!.first { $0["id"]?.string == "drutil-match" }!
        let lines = c["given"]!["lines"]!.array!.map { $0.string! }
        let matches = c["given"]!["match"]!.array!.map { $0.string! }
        let want = c["expect"]!["indices"]!.array!.map { $0.array!.map { Int($0.int!) } }
        #expect(matches.map { PlatformDriveControl.drutilIndices(lines, matching: $0) } == want)
    }

    @Test func theSharedCasesPass() async throws {
        let doc = try Fixtures.json("adapters/drive-control.cases.json")
        let drives = PlatformDriveControl(launcher: PlatformProcessLauncher(), clock: SystemClock())
        var failures: [String] = []
        for c in doc["cases"]?.array ?? [] {
            let given = c["given"]!, expect = c["expect"]!
            do {
                if let open = given["openRaw"]?.string {
                    do {
                        let reader = try drives.openRaw(Fixtures.url(open).path)
                        defer { reader.close() }
                        try Fixtures.check(expect["error"] == nil, "expected \(expect["error"]!)")
                        try Fixtures.same(expect["sectorCount"]?.int, try reader.sectorCount(), "sectorCount")
                        for r in expect["reads"]!.array! {
                            let bytes = try reader.read(r["sector"]!.int!, count: Int(r["count"]!.int!))
                            try Fixtures.same(Int(r["length"]!.int!), bytes.count, "length")
                            if let hex = r["startsHex"]?.string {
                                try Fixtures.check(bytes.map { String(format: "%02x", $0) }.joined().hasPrefix(hex), "starts with \(hex)")
                            }
                        }
                    } catch let error as BroError {
                        try Fixtures.same(expect["error"]?.string, error.code, "error")
                        if let size = expect["size"] { try Fixtures.same(size, JsonValue.object(error.params)["size"] ?? .null, "size") }
                    }
                } else if let probe = given["probeContent"]?.string {
                    try Fixtures.same(expect["content"]!.string!, drives.probeContent(Fixtures.url(probe).path).rawValue, "content")
                } else {
                    let start = Date()
                    let mount = await drives.waitForMount(Fixtures.url(given["waitForMount"]!.string!).path, timeout: Duration(seconds: given["timeout"]!.double!),
                                                          cancel: CancellationToken())
                    try Fixtures.check(mount == nil, "mounted at \(mount!)")
                    try Fixtures.check(Date().timeIntervalSince(start) < expect["within"]!.double!, "within")
                }
            } catch {
                failures.append("\(c["id"]!.string!): \(error)")
            }
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    /// Something that is neither a file nor a disk (a folder) has no size to read: drive.sizeUnknown, never 0 sectors.
    @Test func noSizeIsAnError() {
        let drives = PlatformDriveControl(launcher: PlatformProcessLauncher(), clock: SystemClock())
        do throws(BroError) {
            let reader = try drives.openRaw(NSTemporaryDirectory())
            reader.close()
            Issue.record("opened a folder")
        } catch {
            #expect(error.code == MessageCode.driveSizeUnknown.rawValue)
        }
    }
}
