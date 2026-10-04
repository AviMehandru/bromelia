import BroAdapters
import BroDomain
import BroFoundation
import BroPorts
import BroTestSupport
import Foundation
import Testing

/// A disc made up on the fly: byte i is (i / 2048 + i) mod 256; some sectors can't be read.
final class FakeDisc: SectorReader, @unchecked Sendable {
    private let lock = NSLock()
    var sectors: Int64 = 0
    var unreadable: Set<Int64> = []
    var afterRead: (@Sendable (Int) -> Void)?
    private(set) var reads = 0
    private(set) var closed = false

    static func at(_ i: Int64) -> UInt8 { UInt8((i / 2048 + i) % 256) }

    func read(_ sector: Int64, count: Int) throws(BroError) -> [UInt8] {
        for s in sector..<(sector + Int64(count)) where unreadable.contains(s) {
            throw BroMessage(.fsFailed, [("operation", .string("read")), ("path", .string("/dev/sr0")), ("reason", .string("I/O error"))], severity: .error).toError()
        }
        let n = Int(max(0, min(Int64(count), sectors - sector)))
        let start = sector * 2048
        let data = (0..<(n * 2048)).map { Self.at(start + Int64($0)) }
        let done = lock.withLock { reads += 1; return reads }
        afterRead?(done)
        return data
    }

    func sectorCount() throws(BroError) -> Int64 { sectors }
    func close() { lock.withLock { closed = true } }
}

/// Opens the fake disc (or fails as a missing drive); the rest isn't used.
final class FakeDriveControl: DriveControl, @unchecked Sendable {
    var disc: FakeDisc?
    private(set) var opened = false
    func eject(_ device: String) async throws(BroError) {}
    func closeTray(_ device: String) async throws(BroError) {}
    func waitForMount(_ device: String, timeout: Duration, cancel: CancellationToken) async -> String? { nil }
    func probeContent(_ device: String) -> DiscContent { .unknown }

    func openRaw(_ device: String) throws(BroError) -> any SectorReader {
        opened = true
        guard let disc else { throw BroError(MessageCode.driveNotFound.rawValue) }
        return disc
    }
}

/// shared/fixtures/adapters/data-imager.cases.json.
@Suite(.serialized) struct DataImagerTests {
    let dir = (NSTemporaryDirectory() as NSString).appendingPathComponent("bromelia-image-\(UUID().uuidString)")

    @Test func theSharedCasesPass() async throws {
        let doc = try Fixtures.json("adapters/data-imager.cases.json")
        var failures: [String] = []
        defer { try? FileManager.default.removeItem(atPath: dir) }
        for c in doc["cases"]?.array ?? [] {
            let given = c["given"]!, expect = c["expect"]!
            do {
                try? FileManager.default.removeItem(atPath: dir)
                try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
                let dest = dir + "/disc.iso"
                if given["existing"]?.bool == true { FileManager.default.createFile(atPath: dest, contents: Data("already here".utf8)) }
                let cancel = CancellationToken()
                let disc = FakeDisc()
                disc.sectors = given["sectors"]!.int!
                disc.unreadable = Set((given["unreadable"]?.array ?? []).map { $0.int! })
                if let after = given["cancelAfterChunks"]?.int { disc.afterRead = { n in if n == Int(after) { cancel.cancel() } } }
                let drives = FakeDriveControl()
                drives.disc = given["openFails"]?.bool == true ? nil : disc
                let sink = RecordingSink()
                do {
                    let bytes = try await DataImager(drives: drives, fs: PlatformFileSystem()).copy("/dev/sr0", destIso: dest, sink: sink, cancel: cancel)
                    try Fixtures.check(expect["error"] == nil, "expected \(expect["error"]!)")
                    try Fixtures.same(expect["bytes"]?.int, bytes, "bytes")
                } catch let error as BroError {
                    try Fixtures.same(expect["error"]?["code"]?.string, error.code, "error")
                    for (k, v) in expect["error"]?["params"]?.members ?? [] { try Fixtures.same(v, JsonValue.object(error.params)[k] ?? .null, k) }
                }
                if let want = expect["progress"]?.array {
                    let got = sink.events.compactMap { e -> String? in
                        if case let .progressValue(current, total, max) = e { return "\(current)/\(total)/\(max)" }
                        return nil
                    }
                    try Fixtures.same(want.map { "\($0.array![0].int!)/\($0.array![1].int!)/10000" }, got, "progress")
                }
                if expect["imageMatches"]?.bool == true {
                    let image = [UInt8](try Data(contentsOf: URL(fileURLWithPath: dest)))
                    try Fixtures.same(Int(disc.sectors * 2048), image.count, "size")
                    for (i, b) in image.enumerated() where b != FakeDisc.at(Int64(i)) { throw BroError("byte \(i) differs") }
                }
                if let reads = expect["reads"]?.int {
                    try Fixtures.same(Int(reads), disc.reads, "reads")
                    try Fixtures.check(!drives.opened, "opened")
                }
                if expect["closed"]?.bool == true { try Fixtures.check(disc.closed, "closed") }
                try Fixtures.same(expect["left"]!.array!.map { $0.string! }, try FileManager.default.contentsOfDirectory(atPath: dir).sorted(), "left")
            } catch {
                failures.append("\(c["id"]!.string!): \(error)")
            }
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }
}
