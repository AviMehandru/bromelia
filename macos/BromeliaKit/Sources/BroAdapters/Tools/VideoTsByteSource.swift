import BroDomain
import Foundation

/// The Domain ByteSource over a DVD (shared/fixtures/adapters/video-ts-byte-source.cases.json): VIDEO_TS inside an
/// ISO 9660 / UDF bridge image (or a disc device, read the same way), a VIDEO_TS folder, or the folder that contains one.
/// Reads stop at the end of a file.
public final class VideoTsByteSource: ByteSource, @unchecked Sendable {
    private static let sector: Int64 = 2048

    private let image: Int32 // an image's descriptor, or -1 for a folder
    private var lba: [String: Int64] = [:]
    private var paths: [String: String] = [:]
    private var sizes: [String: Int64] = [:]
    private let volumeLabel: String

    private init(image: Int32, label: String) {
        self.image = image
        volumeLabel = label
    }

    deinit { if image >= 0 { close(image) } }

    /// None when `path` is neither an image with VIDEO_TS nor a folder with (or that is) VIDEO_TS holding VIDEO_TS.IFO.
    public static func open(_ path: String) -> VideoTsByteSource? {
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(atPath: path, isDirectory: &isDirectory) else { return nil }
        return isDirectory.boolValue ? openFolder(path) : openImage(path)
    }

    /// The image's volume name, or the name of the folder that contains VIDEO_TS.
    public func label() -> String { volumeLabel }

    public func files() -> [ByteFile] {
        sizes.map { ByteFile(name: $0.key, size: $0.value) }.sorted { Array($0.name.utf8).lexicographicallyPrecedes(Array($1.name.utf8)) }
    }

    public func read(_ path: String, offset: Int64, length: Int) -> [UInt8] {
        guard let size = sizes[path], offset >= 0, offset < size, length > 0 else { return [] }
        let count = Int(min(Int64(length), size - offset))
        if image >= 0, let start = lba[path] { return Self.readSectors(image, start * Self.sector + offset, count) }
        guard let file = paths[path] else { return [] }
        let fd = Darwin.open(file, O_RDONLY | O_CLOEXEC)
        guard fd >= 0 else { return [] }
        defer { close(fd) }
        return Self.readAt(fd, offset, count)
    }

    private static func openFolder(_ path: String) -> VideoTsByteSource? {
        var dir = path
        while dir.count > 1, dir.hasSuffix("/") { dir.removeLast() }
        if (dir as NSString).lastPathComponent.uppercased() != "VIDEO_TS" {
            guard let sub = (try? FileManager.default.contentsOfDirectory(atPath: dir))?.first(where: { $0.uppercased() == "VIDEO_TS" }) else { return nil }
            dir = (dir as NSString).appendingPathComponent(sub)
        }
        let source = VideoTsByteSource(image: -1, label: ((dir as NSString).deletingLastPathComponent as NSString).lastPathComponent)
        for name in (try? FileManager.default.contentsOfDirectory(atPath: dir)) ?? [] {
            let file = (dir as NSString).appendingPathComponent(name)
            var st = stat()
            guard stat(file, &st) == 0, (st.st_mode & S_IFMT) == S_IFREG else { continue }
            source.paths[name.uppercased()] = file
            source.sizes[name.uppercased()] = Int64(st.st_size)
        }
        return source.sizes["VIDEO_TS.IFO"] != nil ? source : nil
    }

    private static func openImage(_ path: String) -> VideoTsByteSource? {
        let fd = Darwin.open(path, O_RDONLY | O_CLOEXEC)
        guard fd >= 0 else { return nil }
        let pvd = readSectors(fd, 16 * sector, Int(sector))
        guard pvd.count >= 190, Array(pvd[1..<6]) == Array("CD001".utf8) else {
            close(fd)
            return nil
        }
        let label = String(decoding: pvd[40..<72], as: UTF8.self).trimmingCharacters(in: CharacterSet(charactersIn: " \0"))
        let source = VideoTsByteSource(image: fd, label: label) // closes fd from now on
        let root = Array(pvd[156..<190])
        guard let videoTs = directory(fd, lba: u32(root, 2), size: u32(root, 10)).first(where: { $0.name.uppercased() == "VIDEO_TS" }) else { return nil }
        for e in directory(fd, lba: videoTs.lba, size: videoTs.size) {
            source.lba[e.name.uppercased()] = e.lba
            source.sizes[e.name.uppercased()] = e.size
        }
        return source
    }

    /// The entries of an ISO 9660 directory: name (without ";1"), first sector, size.
    private static func directory(_ fd: Int32, lba: Int64, size: Int64) -> [(name: String, lba: Int64, size: Int64)] {
        let data = readSectors(fd, lba * sector, Int(min(size, 1 << 24)))
        var entries: [(String, Int64, Int64)] = []
        var i = 0
        while i < data.count {
            let len = Int(data[i])
            if len == 0 {
                i = (i / Int(sector) + 1) * Int(sector) // records don't cross sectors
                continue
            }
            guard i + len <= data.count, len >= 34 else { break }
            let nameLen = Int(data[i + 32])
            if 33 + nameLen <= len {
                let raw = String(decoding: data[(i + 33)..<(i + 33 + nameLen)], as: UTF8.self)
                let name = String(raw.split(separator: ";", omittingEmptySubsequences: false).first ?? "")
                if name != "\u{0}" && name != "\u{1}" { entries.append((name, u32(data, i + 2), u32(data, i + 10))) }
            }
            i += len
        }
        return entries
    }

    /// Whole sectors around the range, then the range: a disc device only reads whole sectors.
    private static func readSectors(_ fd: Int32, _ offset: Int64, _ count: Int) -> [UInt8] {
        let start = offset / sector * sector, end = (offset + Int64(count) + sector - 1) / sector * sector
        let data = readAt(fd, start, Int(end - start))
        let skip = Int(offset - start)
        return data.count <= skip ? [] : Array(data[skip..<min(data.count, skip + count)])
    }

    private static func readAt(_ fd: Int32, _ offset: Int64, _ count: Int) -> [UInt8] {
        var buffer = [UInt8](repeating: 0, count: count)
        var total = 0
        while total < count {
            let n = buffer.withUnsafeMutableBytes { pread(fd, $0.baseAddress! + total, count - total, off_t(offset) + off_t(total)) }
            if n < 0 && errno == EINTR { continue }
            if n <= 0 { break }
            total += n
        }
        return Array(buffer.prefix(total))
    }

    private static func u32(_ b: [UInt8], _ i: Int) -> Int64 {
        i + 4 <= b.count ? Int64(b[i]) | Int64(b[i + 1]) << 8 | Int64(b[i + 2]) << 16 | Int64(b[i + 3]) << 24 : 0
    }
}
