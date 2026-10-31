import BroDomain
import BroFoundation
import BroPorts
import Foundation

/// Files and folders in memory: what the engine's tests need of the FileSystem port. Thread-safe. While `recording` is on,
/// the calls that change something are written to `ops` ('write', 'sync', 'remove', 'rename', 'syncDirectory', and
/// 'open <path> disk|cache'); `put` and `putFolder` (a tool writing, a case's setup) aren't. `setCached` gives what a read
/// through the cache gives instead of what is on disk; `fail` makes an operation on a path fail before it acts (open,
/// read, sync, syncDirectory, write, remove, rename: its from path); a rename refuses a path that exists and a folder that
/// isn't there, as the real ones do; `onOp` hears of each operation before it acts. Paths are keyed by their
/// UTF-8 bytes, as a disk names them: an NFC and an NFD spelling of one name are two files (Swift's `==` on String
/// would make them one).
public final class MemoryFileSystem: FileSystem, @unchecked Sendable {
    private let lock = NSLock()
    private var files: [[UInt8]: [UInt8]] = [:]
    private var folders: Set<[UInt8]> = [Array("/".utf8)]
    private var volumes: [String: VolumeInfo] = [:]
    private var _ops: [String] = []
    private var _recording = false
    private var cached: [[UInt8]: [UInt8]] = [:]
    private var fails: [(op: String, path: String, error: BroError)] = []
    private var _onOp: (@Sendable (String, String) -> Void)?
    private var _onRead: (@Sendable (String) -> Void)?
    private var _anyVolume: VolumeInfo?
    private var readFails: [[UInt8]: BroError] = [:]
    private var _reads: [String: Int] = [:]
    private var _openStreams = 0

    public init() {}

    public var ops: [String] { lock.withLock { _ops } }
    /// How many pieces of each file were read while `recording` was on.
    public var reads: [String: Int] { lock.withLock { _reads } }
    /// The streams `openForReading` gave that aren't closed yet.
    public var openStreams: Int { lock.withLock { _openStreams } }
    public var onOp: (@Sendable (String, String) -> Void)? {
        get { lock.withLock { _onOp } }
        set { lock.withLock { _onOp = newValue } }
    }
    /// Hears of each whole-file `read` before it reads (a read that is slow, say).
    public var onRead: (@Sendable (String) -> Void)? {
        get { lock.withLock { _onRead } }
        set { lock.withLock { _onRead = newValue } }
    }
    /// What `volume` gives for a path `setVolume` didn't name (nil: fs.notFound).
    public var anyVolume: VolumeInfo? {
        get { lock.withLock { _anyVolume } }
        set { lock.withLock { _anyVolume = newValue } }
    }
    public var recording: Bool {
        get { lock.withLock { _recording } }
        set { lock.withLock { _recording = newValue } }
    }

    /// What a read that doesn't bypass the cache gives for this path.
    public func setCached(_ path: String, _ bytes: [UInt8]) { lock.withLock { cached[Array(path.utf8)] = bytes } }

    /// Makes a whole-file `read` of `path` fail with `error` (C#'s ReadFails).
    public func failReads(_ path: String, _ error: BroError) { lock.withLock { readFails[MemoryFileSystem.key(path)] = error } }

    /// Makes `op` on `path` fail with `error`, before it acts.
    public func fail(_ op: String, _ path: String, _ error: BroError) { lock.withLock { fails.append((op, path, error)) } }

    /// Forgets the failures `fail` set.
    public func clearFails() { lock.withLock { fails.removeAll() } }

    /// Forgets the recorded ops and reads.
    public func clearRecorded() {
        lock.withLock {
            _ops.removeAll()
            _reads.removeAll()
        }
    }

    /// `onOp`, then the failure set for this operation and path, if any.
    private func check(_ op: String, _ path: String) throws(BroError) {
        let (hook, error) = lock.withLock { (_onOp, fails.first { $0.op == op && $0.path == path }?.error) }
        hook?(op, path)
        if let error { throw error }
    }

    /// What `volume` gives for this path.
    public func setVolume(_ path: String, _ info: VolumeInfo) { lock.withLock { volumes[path] = info } }

    private static func key(_ path: String) -> [UInt8] { Array(path.utf8) }

    private static func notFound(_ path: String) -> BroError { BroError("fs.notFound", [("path", .string(path))]) }

    /// Paths split at the byte '/', as the real file systems do (not at Swift's grapheme '/').
    private static func parent(_ path: String) -> String {
        guard let i = path.utf8.lastIndex(of: UInt8(ascii: "/")), i != path.utf8.startIndex else { return "/" }
        return String(decoding: path.utf8[..<i], as: UTF8.self)
    }

    /// The lock held.
    private func record(_ op: String) { if _recording { _ops.append(op) } }

    /// The lock held.
    private func putFolderLocked(_ path: String) {
        var current = ""
        for part in path.utf8.split(separator: UInt8(ascii: "/")) {
            current += "/" + String(decoding: part, as: UTF8.self)
            folders.insert(MemoryFileSystem.key(current))
        }
    }

    /// A file with these bytes, its folders made.
    public func put(_ path: String, _ bytes: [UInt8]) {
        lock.withLock {
            putFolderLocked(MemoryFileSystem.parent(path))
            files[MemoryFileSystem.key(path)] = bytes
        }
    }

    public func putFolder(_ path: String) { lock.withLock { putFolderLocked(path) } }

    public func exists(_ path: String) -> Bool { lock.withLock { files[MemoryFileSystem.key(path)] != nil || folders.contains(MemoryFileSystem.key(path)) } }
    public func read(_ path: String) throws(BroError) -> [UInt8] {
        onRead?(path)
        let (b, fails) = lock.withLock { (files[MemoryFileSystem.key(path)], readFails[MemoryFileSystem.key(path)]) }
        if let fails { throw fails }
        guard let b else { throw MemoryFileSystem.notFound(path) }
        return b
    }
    public func createDirectory(_ path: String, parentsMustExist: Bool) throws(BroError) { putFolder(path) }
    public func writeAtomically(_ path: String, bytes: [UInt8], mode: Int) throws(BroError) {
        try check("write", path)
        let folder = MemoryFileSystem.parent(path)
        let ok = lock.withLock { () -> Bool in
            guard folders.contains(MemoryFileSystem.key(folder)) else { return false }
            files[MemoryFileSystem.key(path)] = bytes
            record("write " + path)
            return true
        }
        if !ok { throw MemoryFileSystem.notFound(folder) }
    }
    public func stat(_ path: String) throws(BroError) -> FileInfo {
        let info = lock.withLock { () -> FileInfo? in
            if let b = files[MemoryFileSystem.key(path)] { return FileInfo(size: Int64(b.count), isDirectory: false, modifiedAt: Instant(unixMilliseconds: 0)) }
            return folders.contains(MemoryFileSystem.key(path)) ? FileInfo(size: 0, isDirectory: true, modifiedAt: Instant(unixMilliseconds: 0)) : nil
        }
        guard let info else { throw MemoryFileSystem.notFound(path) }
        return info
    }
    public func list(_ directory: String) throws(BroError) -> [DirectoryEntry] {
        let entries = lock.withLock { () -> [DirectoryEntry]? in
            guard folders.contains(MemoryFileSystem.key(directory)) else { return nil }
            let prefix = MemoryFileSystem.key(directory == "/" ? "/" : directory + "/")
            func child(_ p: [UInt8]) -> String? {
                guard p.starts(with: prefix), p.count > prefix.count else { return nil }
                let rest = p.dropFirst(prefix.count)
                return rest.contains(UInt8(ascii: "/")) ? nil : String(decoding: rest, as: UTF8.self)
            }
            return (files.keys.compactMap(child).map { DirectoryEntry(name: $0, isDirectory: false) }
                + folders.compactMap(child).map { DirectoryEntry(name: $0, isDirectory: true) })
                .sorted { $0.name.utf8.lexicographicallyPrecedes($1.name.utf8) }
        }
        guard let entries else { throw MemoryFileSystem.notFound(directory) }
        return entries
    }
    public func readRange(_ path: String, offset: Int64, length: Int) throws(BroError) -> [UInt8] {
        let bytes = try read(path)
        guard offset < Int64(bytes.count) else { return [] }
        return Array(bytes[Int(offset)..<min(bytes.count, Int(offset) + length)])
    }
    public func rename(_ from: String, to: String) throws(BroError) {
        try check("rename", from)
        let error = lock.withLock { () -> BroError? in
            let from8 = MemoryFileSystem.key(from), to8 = MemoryFileSystem.key(to), inside = MemoryFileSystem.key(from + "/")
            if files[to8] != nil || folders.contains(to8) { return BroError("fs.alreadyExists", [("path", .string(to))]) }
            let parent = MemoryFileSystem.parent(to)
            if !folders.contains(MemoryFileSystem.key(parent)) { return BroError("fs.parentMissing", [("path", .string(parent))]) }
            if let b = files.removeValue(forKey: from8) {
                files[to8] = b
            } else if folders.contains(from8) {
                for f in files.keys where f.starts(with: inside) {
                    files[to8 + f.dropFirst(from8.count)] = files.removeValue(forKey: f)
                }
                for d in folders where d == from8 || d.starts(with: inside) {
                    folders.remove(d)
                    folders.insert(to8 + d.dropFirst(from8.count))
                }
            } else {
                return MemoryFileSystem.notFound(from)
            }
            record("rename \(from) \(to)")
            return nil
        }
        if let error { throw error }
    }
    public func moveMerging(_ from: String, to: String, policy: MovePolicy, onMoved: (MovedItem) -> Void) throws(BroError) -> [MovedItem] { fatalError("not used") }
    public func remove(_ path: String) throws(BroError) {
        try check("remove", path)
        let error = lock.withLock { () -> BroError? in
            let path8 = MemoryFileSystem.key(path), inside = MemoryFileSystem.key(path + "/")
            if files.removeValue(forKey: path8) != nil {
                record("remove " + path)
                return nil
            }
            guard folders.contains(path8) else { return MemoryFileSystem.notFound(path) }
            if files.keys.contains(where: { $0.starts(with: inside) }) || folders.contains(where: { $0.starts(with: inside) }) {
                return BroError("fs.failed", [("path", .string(path)), ("reason", .string("not empty"))])
            }
            folders.remove(path8)
            record("remove " + path)
            return nil
        }
        if let error { throw error }
    }
    public func moveToTrash(_ path: String, trash: String) throws(BroError) { fatalError("not used") }
    public func syncFile(_ path: String) throws(BroError) {
        try check("sync", path)
        let ok = lock.withLock { () -> Bool in
            guard files[MemoryFileSystem.key(path)] != nil else { return false }
            record("sync " + path)
            return true
        }
        if !ok { throw MemoryFileSystem.notFound(path) }
    }
    public func syncDirectory(_ path: String) throws(BroError) {
        try check("syncDirectory", path)
        let ok = lock.withLock { () -> Bool in
            guard folders.contains(MemoryFileSystem.key(path)) else { return false }
            record("syncDirectory " + path)
            return true
        }
        if !ok { throw MemoryFileSystem.notFound(path) }
    }
    public func openForReading(_ path: String, bypassCache: Bool) throws(BroError) -> any ByteStream {
        try check("open", path)
        let bytes = lock.withLock { () -> [UInt8]? in
            guard let disk = files[MemoryFileSystem.key(path)] else { return nil }
            record("open \(path) \(bypassCache ? "disk" : "cache")")
            _openStreams += 1
            return !bypassCache ? cached[MemoryFileSystem.key(path)] ?? disk : disk
        }
        guard let bytes else { throw MemoryFileSystem.notFound(path) }
        return Stream(fs: self, path: path, bytes: bytes)
    }

    /// A file read a few bytes at a time ('read' fails are checked on each read).
    private final class Stream: ByteStream, @unchecked Sendable {
        private let fs: MemoryFileSystem
        private let path: String
        private let bytes: [UInt8]
        private var at = 0
        private var closed = false

        init(fs: MemoryFileSystem, path: String, bytes: [UInt8]) {
            self.fs = fs
            self.path = path
            self.bytes = bytes
        }

        func read(_ maxBytes: Int) throws(BroError) -> [UInt8] {
            try fs.check("read", path)
            fs.lock.withLock { if fs._recording { fs._reads[path, default: 0] += 1 } }
            let n = min(maxBytes, 16, bytes.count - at)
            let piece = Array(bytes[at..<at + n])
            at += n
            return piece
        }

        func close() {
            fs.lock.withLock {
                if !closed {
                    closed = true
                    fs._openStreams -= 1
                }
            }
        }
    }
    public func volume(_ path: String) throws(BroError) -> VolumeInfo {
        guard let v = lock.withLock({ volumes[path] ?? _anyVolume }) else { throw MemoryFileSystem.notFound(path) }
        return v
    }
}
