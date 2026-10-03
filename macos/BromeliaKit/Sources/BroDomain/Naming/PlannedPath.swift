/// A path relative to the library (folders separated by `/`), with its role and the title or episode it holds (the
/// API's PlannedPath).
public struct PlannedPath: Sendable, Equatable {
    public var path: String
    public var role: PathRole
    public var title: Int?
    public var episode: Int?

    public init(path: String, role: PathRole, title: Int? = nil, episode: Int? = nil) {
        self.path = path
        self.role = role
        self.title = title
        self.episode = episode
    }
}
