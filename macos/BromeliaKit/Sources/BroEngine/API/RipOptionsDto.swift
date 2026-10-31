import BroDomain

/// How a client asks for a rip (dtos.json RipOptions): the mode, profile and library to use instead of what the rules
/// give, the titles of the open session, and a one-off output folder that must be a library's.
public struct RipOptionsDto: Sendable, Equatable {
    public var mode: RipMode?
    public var profile: String?
    public var library: String?
    public var titles: [Int]?
    public var outputFolder: String?

    public init(mode: RipMode? = nil, profile: String? = nil, library: String? = nil, titles: [Int]? = nil, outputFolder: String? = nil) {
        self.mode = mode
        self.profile = profile
        self.library = library
        self.titles = titles
        self.outputFolder = outputFolder
    }
}
