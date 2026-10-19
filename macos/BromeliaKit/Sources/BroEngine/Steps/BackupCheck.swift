import BroDomain
import BroFoundation
import BroPorts

/// Whether a backup a job made looks like a disc (BackupStructure.problem), read from disk: a folder by its files, an
/// image by its volume descriptor at byte 32769.
public enum BackupCheck {
    /// What is wrong with the backup at `path` (named by its last part), or nil: structure.notCreated when nothing is
    /// there, structure.unreadable when it can't be read.
    public static func problem(_ fs: any FileSystem, path: String) -> BroMessage? {
        let name = path.split(separator: "/").last.map(String.init) ?? path
        if !fs.exists(path) { return BackupStructure.problem(name, iso: false, entries: nil, isoHeader: nil) }
        do throws(BroError) {
            return try fs.stat(path).isDirectory
                ? BackupStructure.problem(name, iso: false, entries: try FileTree.files(fs, root: path), isoHeader: nil)
                : BackupStructure.problem(name, iso: true, entries: nil, isoHeader: try fs.readRange(path, offset: 32769, length: 5))
        } catch {
            return BroMessage(.structureUnreadable, [("name", .string(name))])
        }
    }
}
