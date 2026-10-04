import BroAdapters
import BroDomain
import BroFoundation
import BroPorts
import BroTestSupport
import Foundation
import Testing

/// shared/fixtures/adapters/file-system.cases.json, and the file-system cases of platform-adapters.contract.json,
/// on the real macOS.
struct PlatformFileSystemTests {
    let fs = PlatformFileSystem()
    /// Resolved (/private/var/…), so the paths in errors are the ones the cases are built under.
    let root = URL(fileURLWithPath: NSTemporaryDirectory()).resolvingSymlinksInPath().path + "/bromelia-fs-" + UUID().uuidString

    func p(_ relative: String) -> String { relative == "." ? root : root + "/" + relative }

    func rel(_ path: String) -> String { String(path.dropFirst(root.count + 1)) }

    func build(_ tree: JsonValue) throws {
        try? FileManager.default.removeItem(atPath: root)
        try FileManager.default.createDirectory(atPath: root, withIntermediateDirectories: true)
        for (name, content) in tree.members ?? [] {
            if name.hasSuffix("/") {
                try FileManager.default.createDirectory(atPath: p(String(name.dropLast())), withIntermediateDirectories: true)
            } else {
                try Data((content.string ?? "").utf8).write(to: URL(fileURLWithPath: p(name)))
            }
        }
    }

    /// The tree under the root, in the cases' notation, sorted by name.
    func tree() -> String {
        var lines: [(String, String)] = []
        let e = FileManager.default.enumerator(atPath: root)!
        while let name = e.nextObject() as? String {
            var isDir: ObjCBool = false
            FileManager.default.fileExists(atPath: p(name), isDirectory: &isDir)
            lines.append(isDir.boolValue ? (name + "/", "(folder)") : (name, (try? String(contentsOfFile: p(name), encoding: .utf8)) ?? "?"))
        }
        return lines.sorted { Array($0.0.utf8).lexicographicallyPrecedes(Array($1.0.utf8)) }.map { "\($0.0) = \($0.1)" }.joined(separator: "\n")
    }

    func run(_ op: [JsonValue]) throws(BroError) -> JsonValue {
        func s(_ i: Int) -> String { op[i].string ?? "" }
        switch s(0) {
        case "exists": return .bool(fs.exists(p(s(1))))
        case "stat":
            let info = try fs.stat(p(s(1)))
            return info.isDirectory ? .object([("isDirectory", .bool(true))]) : .object([("size", .integer(info.size)), ("isDirectory", .bool(false))])
        case "list":
            let names = try fs.list(p(s(1))).map { $0.name + ($0.isDirectory ? "/" : "") }
            return .array(names.sorted { Array($0.utf8).lexicographicallyPrecedes(Array($1.utf8)) }.map { .string($0) })
        case "read": return .string(String(decoding: try fs.read(p(s(1))), as: UTF8.self))
        case "readRange": return .string(String(decoding: try fs.readRange(p(s(1)), offset: op[2].int!, length: Int(op[3].int!)), as: UTF8.self))
        case "createDirectory": try fs.createDirectory(p(s(1)), parentsMustExist: op[2].bool!)
        case "writeAtomically": try fs.writeAtomically(p(s(1)), bytes: Array(s(2).utf8), mode: Int(op[3].int!))
        case "rename": try fs.rename(p(s(1)), to: p(s(2)))
        case "moveMerging":
            return .array(try fs.moveMerging(p(s(1)), to: p(s(2)), policy: MovePolicy(rawValue: s(3))!).map { .array([.string(rel($0.from)), .string(rel($0.to))]) })
        case "remove": try fs.remove(p(s(1)))
        case "moveToTrash": try fs.moveToTrash(p(s(1)), trash: p(s(2)))
        case "syncFile": try fs.syncFile(p(s(1)))
        case "syncDirectory": try fs.syncDirectory(p(s(1)))
        case "openForReading":
            let stream = try fs.openForReading(p(s(1)), bypassCache: op[2].bool!)
            var all: [UInt8] = []
            while true {
                let chunk = try stream.read(4)
                if chunk.isEmpty { break }
                all += chunk
            }
            stream.close()
            return .string(String(decoding: all, as: UTF8.self))
        default: return .string("unknown operation \(s(0))")
        }
        return .null
    }

    @Test func theSharedCasesPass() throws {
        defer { try? FileManager.default.removeItem(atPath: root) }
        let failures = try Fixtures.runCases("adapters/file-system.cases.json") { _, given, expect in
            try build(given["tree"]!)
            let op = given["op"]!.array!
            if let error = expect["error"] {
                do {
                    _ = try run(op)
                    throw FixtureError("no error; expected \(error)")
                } catch let e as BroError {
                    try Fixtures.same(error["code"]!.string!, e.code, "code")
                    for (key, value) in error["params"]?.members ?? [] {
                        var actual = JsonValue.object(e.params)[key]?.string ?? ""
                        if key == "path" { actual = "<root>/" + rel(actual) }
                        try Fixtures.same(value.string!, actual, key)
                    }
                }
            } else {
                let result = try run(op)
                if let want = expect["result"] {
                    if let members = want.members, let got = result.members {
                        for (k, v) in members { try Fixtures.same(v, got.first { $0.key == k }?.value ?? .null, k) }
                    } else {
                        try Fixtures.same(want, result, "result")
                    }
                }
            }
            if let want = expect["tree"] {
                let wantText = (want.members ?? []).sorted { Array($0.key.utf8).lexicographicallyPrecedes(Array($1.key.utf8)) }
                    .map { "\($0.key) = \($0.value.string ?? "(folder)")" }.joined(separator: "\n")
                try Fixtures.same(wantText, tree(), "tree")
            }
            return true
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    /// file-system-durable-write, file-system-read-uncached, and the permission bits.
    @Test func writesAreDurableKeepTheModeAndReadsCanBypassTheCache() throws {
        defer { try? FileManager.default.removeItem(atPath: root) }
        try FileManager.default.createDirectory(atPath: root, withIntermediateDirectories: true)
        var big = [UInt8](repeating: 0, count: 3 << 20 | 123)
        for i in big.indices { big[i] = UInt8(truncatingIfNeeded: i &* 2654435761 >> 13) }
        let path = p("big.bin")
        try fs.writeAtomically(path, bytes: big, mode: 0o600)
        try fs.syncFile(path)
        try fs.syncDirectory(root)
        let mode = (try FileManager.default.attributesOfItem(atPath: path)[.posixPermissions] as? NSNumber)?.intValue
        #expect(mode == 0o600)
        let stream = try fs.openForReading(path, bypassCache: true)
        var all: [UInt8] = []
        while true {
            let chunk = try stream.read(100_000)
            if chunk.isEmpty { break }
            all += chunk
        }
        stream.close()
        #expect(all == big)
        #expect(try FileManager.default.contentsOfDirectory(atPath: root) == ["big.bin"])
    }

    @Test func theVolumeHasAnIdFreeSpaceAndAType() throws {
        defer { try? FileManager.default.removeItem(atPath: root) }
        try FileManager.default.createDirectory(atPath: root, withIntermediateDirectories: true)
        let v = try fs.volume(p("not/yet/there"))
        #expect(v.id.count == 16)
        #expect(v.freeBytes > 0)
        #expect(v.fsType == "apfs")
        #expect(v.caseSensitive == false)
        #expect(try fs.volume(root).id == v.id)
    }
}
