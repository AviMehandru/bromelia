import BroDomain
import BroFoundation

/// A row of unit_files (the unit is implied).
public struct UnitFile: Sendable, Equatable {
    /// Relative to the unit's folder.
    public var path: String
    public var size: Int64
    public var sha256: String
    public var role: String
    public var title: Int?
    public var episode: Int?

    public init(path: String, size: Int64, sha256: String, role: String, title: Int? = nil, episode: Int? = nil) {
        self.path = path
        self.size = size
        self.sha256 = sha256
        self.role = role
        self.title = title
        self.episode = episode
    }
}
