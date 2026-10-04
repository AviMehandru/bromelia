import BroDomain
import BroFoundation

/// The volume holding a path: a stable id, the bytes free, the file system and whether names are case-sensitive.
public struct VolumeInfo: Sendable, Equatable {
    public var id: String
    public var freeBytes: Int64
    public var fsType: String
    public var caseSensitive: Bool

    public init(id: String, freeBytes: Int64, fsType: String, caseSensitive: Bool) {
        self.id = id
        self.freeBytes = freeBytes
        self.fsType = fsType
        self.caseSensitive = caseSensitive
    }
}
