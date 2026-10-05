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
    var sizeFails = false
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

    func sectorCount() throws(BroError) -> Int64 {
        if sizeFails {
            throw BroMessage(.driveSizeUnknown, [("path", .string("/dev/sr0")), ("reason", .string("Inappropriate ioctl for device"))], severity: .error).toError()
        }
        return sectors
    }
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

/// The real file system, except that folder syncs fail, as on a share that refuses them.
final class SyncFailingFileSystem: FileSystem, @unchecked Sendable {
    private let real = PlatformFileSystem()
    func exists(_ path: String) -> Bool { real.exists(path) }
    func stat(_ path: String) throws(BroError) -> FileInfo { try real.stat(path) }
    func list(_ directory: String) throws(BroError) -> [DirectoryEntry] { try real.list(directory) }
    func read(_ path: String) throws(BroError) -> [UInt8] { try real.read(path) }
    func readRange(_ path: String, offset: Int64, length: Int) throws(BroError) -> [UInt8] { try real.readRange(path, offset: offset, length: length) }
    func createDirectory(_ path: String, parentsMustExist: Bool) throws(BroError) { try real.createDirectory(path, parentsMustExist: parentsMustExist) }
    func writeAtomically(_ path: String, bytes: [UInt8], mode: Int) throws(BroError) { try real.writeAtomically(path, bytes: bytes, mode: mode) }
    func rename(_ from: String, to: String) throws(BroError) { try real.rename(from, to: to) }
    func moveMerging(_ from: String, to: String, policy: MovePolicy, onMoved: (MovedItem) -> Void) throws(BroError) -> [MovedItem] {
        try real.moveMerging(from, to: to, policy: policy, onMoved: onMoved)
    }
    func remove(_ path: String) throws(BroError) { try real.remove(path) }
    func moveToTrash(_ path: String, trash: String) throws(BroError) { try real.moveToTrash(path, trash: trash) }
    func syncFile(_ path: String) throws(BroError) { try real.syncFile(path) }
    func syncDirectory(_ path: String) throws(BroError) {
        throw BroMessage(.fsFailed, [("operation", .string("sync")), ("path", .string(path)), ("reason", .string("Operation not supported"))],
                         severity: .error).toError()
    }
    func openForReading(_ path: String, bypassCache: Bool) throws(BroError) -> any ByteStream { try real.openForReading(path, bypassCache: bypassCache) }
    func volume(_ path: String) throws(BroError) -> VolumeInfo { try real.volume(path) }
}

/// shared/fixtures/adapters/data-imager.cases.json.
@Suite(.serialized) struct DataImagerTests {
    let dir = (Fixtures.temporaryDirectory as NSString).appendingPathComponent("bromelia-image-\(UUID().uuidString)")

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
                let cancelSource = CancellationSource()
                let cancel = cancelSource.token
                let disc = FakeDisc()
                disc.sectors = given["sectors"]!.int!
                disc.sizeFails = given["sizeFails"]?.bool == true
                disc.unreadable = Set((given["unreadable"]?.array ?? []).map { $0.int! })
                if let after = given["cancelAfterChunks"]?.int { disc.afterRead = { n in if n == Int(after) { cancelSource.cancel() } } }
                let drives = FakeDriveControl()
                drives.disc = given["openFails"]?.bool == true ? nil : disc
                let sink = RecordingSink()
                do {
                    let fs: any FileSystem = given["syncFails"]?.bool == true ? SyncFailingFileSystem() : PlatformFileSystem()
                    let copy = try await DataImager(drives: drives, fs: fs).copy("/dev/sr0", destIso: dest, sink: sink, cancel: cancel)
                    try Fixtures.check(expect["error"] == nil, "expected \(expect["error"]!)")
                    try Fixtures.same(expect["bytes"]?.int, copy.bytes, "bytes")
                    try Fixtures.same(expect["warning"]?.string, copy.warning?.code.rawValue, "warning")
                    if let w = copy.warning { try Fixtures.same(JsonValue.string(dest), JsonValue.object(w.params)["path"] ?? .null, "warning path") }
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
