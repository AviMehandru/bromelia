import BroDomain
import BroFoundation

/// A row of commit_items.
public struct CommitItem: Sendable, Equatable {
    public var seq: Int
    /// Relative to staging.
    public var fromPath: String
    /// Relative to the destination.
    public var toPath: String
    /// Files only.
    public var sha256: String?
    public var isDir: Bool
    public var moved: Bool

    public init(seq: Int, fromPath: String, toPath: String, sha256: String? = nil, isDir: Bool, moved: Bool) {
        self.seq = seq
        self.fromPath = fromPath
        self.toPath = toPath
        self.sha256 = sha256
        self.isDir = isDir
        self.moved = moved
    }
}
