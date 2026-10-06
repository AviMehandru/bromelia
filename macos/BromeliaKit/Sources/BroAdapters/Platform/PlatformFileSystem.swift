import BroDomain
import BroFoundation
import BroPorts
import Darwin
import Foundation

/// The FileSystem port on macOS (plan §9, §10.3; shared/fixtures/adapters/file-system.cases.json): writes are
/// flushed with F_FULLFSYNC (the file, then its folder), renames never replace (renamex_np with RENAME_EXCL) except
/// writeAtomically's, a missing parent is created only when asked, uncached reads use F_NOCACHE, and the volume comes
/// from statfs. A file system without RENAME_EXCL (an SMB share) gets the Linux fallback: link + unlink for a file,
/// else a rename only after checking nothing is there.
public final class PlatformFileSystem: FileSystem {
    public init() {}

    public func exists(_ path: String) -> Bool {
        var st = Darwin.stat()
        return lstat(path, &st) == 0
    }

    public func stat(_ path: String) throws(BroError) -> FileInfo {
        var st = Darwin.stat()
        guard sysStat(path, &st) == 0 else { throw errnoError("stat", path) }
        let ms = Int64(st.st_mtimespec.tv_sec) * 1000 + Int64(st.st_mtimespec.tv_nsec) / 1_000_000
        let isDirectory = (st.st_mode & S_IFMT) == S_IFDIR
        return FileInfo(size: isDirectory ? 0 : Int64(st.st_size), isDirectory: isDirectory, modifiedAt: Instant(unixMilliseconds: ms))
    }

    public func list(_ directory: String) throws(BroError) -> [DirectoryEntry] {
        guard let dir = opendir(directory) else { throw errnoError("list", directory) }
        defer { closedir(dir) }
        var entries: [DirectoryEntry] = []
        while let e = readdir(dir) {
            let name = withUnsafeBytes(of: e.pointee.d_name) { raw in
                String(decoding: raw.prefix(Int(e.pointee.d_namlen)), as: UTF8.self)
            }
            if name == "." || name == ".." { continue }
            var isDirectory = e.pointee.d_type == DT_DIR
            if e.pointee.d_type == DT_UNKNOWN || e.pointee.d_type == DT_LNK {
                var st = Darwin.stat()
                isDirectory = sysStat(directory + "/" + name, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR
            }
            entries.append(DirectoryEntry(name: name, isDirectory: isDirectory))
        }
        return entries
    }

    public func read(_ path: String) throws(BroError) -> [UInt8] {
        let fd = try openFile(path, "read")
        defer { close(fd) }
        var out: [UInt8] = []
        var buffer = [UInt8](repeating: 0, count: 1 << 16)
        while true {
            let n = buffer.withUnsafeMutableBytes { Darwin.read(fd, $0.baseAddress, $0.count) }
            if n < 0 && errno == EINTR { continue }
            if n < 0 { throw errnoError("read", path) }
            if n == 0 { return out }
            out.append(contentsOf: buffer[0..<n])
        }
    }

    public func readRange(_ path: String, offset: Int64, length: Int) throws(BroError) -> [UInt8] {
        let fd = try openFile(path, "read")
        defer { close(fd) }
        var buffer = [UInt8](repeating: 0, count: max(0, length))
        var got = 0
        while got < length {
            let n = buffer.withUnsafeMutableBytes { pread(fd, $0.baseAddress! + got, length - got, off_t(offset) + off_t(got)) }
            if n < 0 && errno == EINTR { continue }
            if n < 0 { throw errnoError("read", path) }
            if n == 0 { break }
            got += n
        }
        return Array(buffer[0..<got])
    }

    public func createDirectory(_ path: String, parentsMustExist: Bool) throws(BroError) {
        if isDirectory(path) { return }
        if exists(path) { throw fsError(.fsAlreadyExists, path) }
        let parent = (path as NSString).deletingLastPathComponent
        if !isDirectory(parent) {
            if parentsMustExist { throw fsError(.fsParentMissing, parent) }
            try createDirectory(parent, parentsMustExist: false)
        }
        if mkdir(path, 0o777) != 0 && !(errno == EEXIST && isDirectory(path)) { throw errnoError("createDirectory", path) }
    }

    public func writeAtomically(_ path: String, bytes: [UInt8], mode: Int) throws(BroError) {
        let parent = (path as NSString).deletingLastPathComponent
        guard isDirectory(parent) else { throw fsError(.fsParentMissing, parent) }
        let temp = parent + "/." + (path as NSString).lastPathComponent + ".bromelia-tmp-" + String(UInt32.random(in: 0...UInt32.max), radix: 16)
        let fd = open(temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode_t(mode))
        guard fd >= 0 else { throw errnoError("write", path) }
        var renamed = false
        defer { if !renamed { unlink(temp) } }
        do {
            defer { close(fd) }
            var written = 0
            while written < bytes.count {
                let n = bytes.withUnsafeBytes { Darwin.write(fd, $0.baseAddress! + written, bytes.count - written) }
                if n < 0 && errno == EINTR { continue }
                if n <= 0 { throw errnoError("write", path) }
                written += n
            }
            fchmod(fd, mode_t(mode))   // whatever the umask took away
            if fcntl(fd, F_FULLFSYNC) != 0 && fsync(fd) != 0 { throw errnoError("write", path) }
        }
        guard Darwin.rename(temp, path) == 0 else { throw errnoError("write", path) }
        renamed = true
        try syncDirectory(parent)
    }

    public func rename(_ from: String, to: String) throws(BroError) {
        guard exists(from) else { throw fsError(.fsNotFound, from) }
        if exists(to) { throw fsError(.fsAlreadyExists, to) }
        let parent = (to as NSString).deletingLastPathComponent
        guard isDirectory(parent) else { throw fsError(.fsParentMissing, parent) }
        // RENAME_EXCL: a file that appears meanwhile is kept, never replaced.
        guard Self.renameNoReplace(from, to) == 0 else {
            if errno == EEXIST { throw fsError(.fsAlreadyExists, to) }
            throw errnoError("rename", from)
        }
    }

    /// renamex_np with RENAME_EXCL. macOS's SMB client doesn't offer it (ENOTSUP): then link + unlink, which never
    /// replaces either, and where links aren't offered (SMB again) a check that nothing is there, then rename. That last
    /// step could replace a file created in between; Bromelia's names are unique to it, so nothing else makes them.
    static func renameNoReplace(_ from: String, _ to: String) -> Int32 {
        if renamex_np(from, to, UInt32(RENAME_EXCL)) == 0 { return 0 }
        guard errno == ENOTSUP || errno == EINVAL else { return -1 }
        var st = Darwin.stat()
        if lstat(from, &st) == 0, (st.st_mode & S_IFMT) != S_IFDIR, link(from, to) == 0 { return unlink(from) }
        if errno == EEXIST { return -1 }
        if lstat(to, &st) == 0 {
            errno = EEXIST
            return -1
        }
        return Darwin.rename(from, to)
    }

    public func moveMerging(_ from: String, to: String, policy: MovePolicy, onMoved: (MovedItem) -> Void) throws(BroError) -> [MovedItem] {
        guard isDirectory(from) else {
            if exists(from) { throw failed("move", from, "it isn't a folder") }
            throw fsError(.fsNotFound, from)
        }
        if !exists(to) {
            try createDirectory(to, parentsMustExist: true)
        } else if !isDirectory(to) {
            throw fsError(.fsAlreadyExists, to)
        }
        var moved: [MovedItem] = []
        try merge(from, into: to) { item in
            moved.append(item)
            onMoved(item)
        }
        return moved.sorted { Array($0.from.utf8).lexicographicallyPrecedes(Array($1.from.utf8)) }
    }

    /// Each entry of source in turn: a folder merges into a folder of the same name (ignoring ASCII case); anything
    /// else moves under ConflictNamer's name. Emptied source folders go.
    private func merge(_ source: String, into target: String, _ moved: (MovedItem) -> Void) throws(BroError) {
        let entries = try list(source).sorted { Array($0.name.utf8).lexicographicallyPrecedes(Array($1.name.utf8)) }
        for e in entries {
            let src = source + "/" + e.name
            let existing = try list(target)
            let same = existing.first { asciiLower($0.name) == asciiLower(e.name) }
            if e.isDirectory, let same, same.isDirectory {
                try merge(src, into: target + "/" + same.name, moved)
                try remove(src)
                continue
            }
            let dst = target + "/" + ConflictNamer.next(e.name, existing: existing.map(\.name), isFolder: e.isDirectory)
            try rename(src, to: dst)
            if e.isDirectory {
                for file in try files(under: dst) { moved(MovedItem(from: src + file, to: dst + file)) }
            } else {
                moved(MovedItem(from: src, to: dst))
            }
        }
    }

    /// Every file under a folder, as "/relative/path".
    private func files(under dir: String) throws(BroError) -> [String] {
        var out: [String] = []
        for e in try list(dir) {
            if e.isDirectory {
                out += try files(under: dir + "/" + e.name).map { "/" + e.name + $0 }
            } else {
                out.append("/" + e.name)
            }
        }
        return out
    }

    public func remove(_ path: String) throws(BroError) {
        if isDirectory(path) {
            guard rmdir(path) == 0 else {
                if errno == ENOTEMPTY || errno == EEXIST { throw failed("remove", path, "the folder isn't empty") }
                throw errnoError("remove", path)
            }
        } else if exists(path) {
            guard unlink(path) == 0 else { throw errnoError("remove", path) }
        } else {
            throw fsError(.fsNotFound, path)
        }
    }

    public func moveToTrash(_ path: String, trash: String) throws(BroError) {
        guard exists(path) else { throw fsError(.fsNotFound, path) }
        try createDirectory(trash, parentsMustExist: true)
        let name = ConflictNamer.next((path as NSString).lastPathComponent, existing: try list(trash).map(\.name), isFolder: isDirectory(path))
        try rename(path, to: trash + "/" + name)
    }

    public func syncFile(_ path: String) throws(BroError) {
        let fd = try openFile(path, "sync")
        defer { close(fd) }
        if fcntl(fd, F_FULLFSYNC) != 0 && fsync(fd) != 0 { throw errnoError("sync", path) }
    }

    /// F_FULLFSYNC on the folder: the rename that just happened in it reaches the disk. A file system that can't
    /// sync a folder at all (EINVAL, ENOTSUP) has nothing to do: the rename is already its to keep.
    public func syncDirectory(_ path: String) throws(BroError) {
        let fd = open(path, O_RDONLY | O_CLOEXEC)
        guard fd >= 0 else { throw errnoError("sync", path) }
        defer { close(fd) }
        if fcntl(fd, F_FULLFSYNC) != 0 && fsync(fd) != 0 && errno != EINVAL && errno != ENOTSUP { throw errnoError("sync", path) }
    }

    public func openForReading(_ path: String, bypassCache: Bool) throws(BroError) -> any ByteStream {
        let fd = try openFile(path, "read")
        if bypassCache { _ = fcntl(fd, F_NOCACHE, 1) }
        return FileStream(fd: fd, path: path)
    }

    /// The volume holding path, or its nearest existing parent: statfs's file system id, the bytes free for this
    /// user, the file system's name, and whether names are case-sensitive.
    public func volume(_ path: String) throws(BroError) -> VolumeInfo {
        var existing = path
        while !exists(existing) && existing != "/" && !existing.isEmpty { existing = (existing as NSString).deletingLastPathComponent }
        var fs = statfs()
        guard statfs(existing, &fs) == 0 else { throw errnoError("volume", path) }
        let id = String(format: "%08x%08x", UInt32(bitPattern: fs.f_fsid.val.0), UInt32(bitPattern: fs.f_fsid.val.1))
        let type = withUnsafeBytes(of: fs.f_fstypename) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
        return VolumeInfo(id: id, freeBytes: Int64(fs.f_bavail) * Int64(fs.f_bsize), fsType: type,
                          caseSensitive: pathconf(existing, _PC_CASE_SENSITIVE) == 1)
    }

    // ---- helpers ---------------------------------------------------------------------------------

    private func isDirectory(_ path: String) -> Bool {
        var st = Darwin.stat()
        return sysStat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR
    }

    private func openFile(_ path: String, _ operation: String) throws(BroError) -> Int32 {
        if isDirectory(path) { throw failed(operation, path, "it is a folder") }
        let fd = open(path, O_RDONLY | O_CLOEXEC)
        guard fd >= 0 else { throw errnoError(operation, path) }
        return fd
    }

    private func asciiLower(_ s: String) -> [UInt8] {
        s.utf8.map { $0 >= 65 && $0 <= 90 ? $0 + 32 : $0 }
    }

    private func fsError(_ code: MessageCode, _ path: String) -> BroError {
        BroMessage(code, [("path", .string(path))], severity: .error).toError()
    }

    private func failed(_ operation: String, _ path: String, _ reason: String) -> BroError {
        BroMessage(.fsFailed, [("operation", .string(operation)), ("path", .string(path)), ("reason", .string(reason))], severity: .error).toError()
    }

    /// The error for errno: fs.notFound for ENOENT, else fs.failed with the system's text.
    private func errnoError(_ operation: String, _ path: String) -> BroError {
        let code = errno
        return code == ENOENT ? fsError(.fsNotFound, path) : failed(operation, path, String(cString: strerror(code)))
    }
}

/// stat(2): inside PlatformFileSystem the name is its own method.
private func sysStat(_ path: String, _ st: inout Darwin.stat) -> Int32 { stat(path, &st) }

/// A file read in chunks (hashing).
private final class FileStream: ByteStream, @unchecked Sendable {
    private let lock = NSLock()
    private var fd: Int32
    private let path: String

    init(fd: Int32, path: String) {
        self.fd = fd
        self.path = path
    }

    func read(_ maxBytes: Int) throws(BroError) -> [UInt8] {
        lock.lock()
        defer { lock.unlock() }
        do {
            guard fd >= 0 else { return [] }
            var buffer = [UInt8](repeating: 0, count: maxBytes)
            while true {
                let n = buffer.withUnsafeMutableBytes { Darwin.read(fd, $0.baseAddress, $0.count) }
                if n < 0 && errno == EINTR { continue }
                if n < 0 {
                    throw BroMessage(.fsFailed, [("operation", .string("read")), ("path", .string(path)),
                                                 ("reason", .string(String(cString: strerror(errno))))], severity: .error).toError()
                }
                return Array(buffer[0..<n])
            }
        }
    }

    func close() {
        lock.withLock {
            if fd >= 0 { Darwin.close(fd) }
            fd = -1
        }
    }
}
