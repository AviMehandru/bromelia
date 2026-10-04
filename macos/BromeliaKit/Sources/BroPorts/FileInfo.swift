import BroDomain
import BroFoundation

/// A file's size, whether it is a folder, and when it last changed.
public struct FileInfo: Sendable, Equatable {
    public var size: Int64
    public var isDirectory: Bool
    public var modifiedAt: Instant

    public init(size: Int64, isDirectory: Bool, modifiedAt: Instant) {
        self.size = size
        self.isDirectory = isDirectory
        self.modifiedAt = modifiedAt
    }
}
