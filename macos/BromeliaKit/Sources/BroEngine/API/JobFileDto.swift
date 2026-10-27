/// A file a job made, as JobDetail lists it (dtos.json JobDetail.files): Seal's (path in the unit folder, size and
/// SHA-256 as read back from disk, role), else Acquire's (path in staging only).
public struct JobFileDto: Sendable, Equatable {
    public var path: String
    public var size: Int64?
    public var sha256: String?
    public var role: String?

    public init(path: String, size: Int64? = nil, sha256: String? = nil, role: String? = nil) {
        self.path = path
        self.size = size
        self.sha256 = sha256
        self.role = role
    }
}
