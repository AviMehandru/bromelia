import BroDomain
import BroFoundation

/// Files and folders. Paths are absolute.
public protocol FileSystem: Sendable {
    /// Whether anything is at path.
    func exists(_ path: String) -> Bool

    func stat(_ path: String) throws(BroError) -> FileInfo

    /// The entries of a folder, in no particular order.
    func list(_ directory: String) throws(BroError) -> [DirectoryEntry]

    func read(_ path: String) throws(BroError) -> [UInt8]

    /// Fewer bytes at the end of the file.
    func readRange(_ path: String, offset: Int64, length: Int) throws(BroError) -> [UInt8]

    /// Creates a folder (and, unless parentsMustExist, its parents). Never creates a library root (plan §22).
    func createDirectory(_ path: String, parentsMustExist: Bool) throws(BroError)

    /// A temporary file, fsync, rename over path, fsync of the folder; mode is the POSIX permission bits. A folder sync
    /// that fails fails the call although the new content is in place: a retry replaces it, so it is safe.
    func writeAtomically(_ path: String, bytes: [UInt8], mode: Int) throws(BroError)

    func rename(_ from: String, to: String) throws(BroError)

    /// Moves a folder's contents into another, merging folders. `onMoved` hears of each item as soon as it has moved,
    /// so a failure part-way (thrown) still says what moved.
    func moveMerging(_ from: String, to: String, policy: MovePolicy, onMoved: (MovedItem) -> Void) throws(BroError) -> [MovedItem]

    /// Removes a file or an empty folder.
    func remove(_ path: String) throws(BroError)

    /// Moves path into the trash folder (never deletes).
    func moveToTrash(_ path: String, trash: String) throws(BroError)

    /// F_FULLFSYNC / FlushFileBuffers / fsync.
    func syncFile(_ path: String) throws(BroError)

    func syncDirectory(_ path: String) throws(BroError)

    /// F_NOCACHE / NO_BUFFERING / fadvise when bypassCache.
    func openForReading(_ path: String, bypassCache: Bool) throws(BroError) -> any ByteStream

    func volume(_ path: String) throws(BroError) -> VolumeInfo
}

extension FileSystem {
    /// moveMerging without hearing of each item.
    public func moveMerging(_ from: String, to: String, policy: MovePolicy) throws(BroError) -> [MovedItem] {
        try moveMerging(from, to: to, policy: policy, onMoved: { _ in })
    }
}
