/// One file a job will write, for `Layouts.paths`: its role, title or episode, the tokens of that file (track,
/// episode, episodeNumber, episodeTitle, original, n, …), whether it's a movie's main feature, and its extension
/// (`.mkv`; empty for a folder).
public struct PlannedOutput: Sendable, Equatable {
    public var role: PathRole
    public var values: [String: String]
    public var title: Int?
    public var episode: Int?
    public var mainFeature: Bool
    public var `extension`: String

    public init(role: PathRole, values: [String: String] = [:], title: Int? = nil, episode: Int? = nil, mainFeature: Bool = false,
                extension: String = "") {
        self.role = role
        self.values = values
        self.title = title
        self.episode = episode
        self.mainFeature = mainFeature
        self.extension = `extension`
    }
}
