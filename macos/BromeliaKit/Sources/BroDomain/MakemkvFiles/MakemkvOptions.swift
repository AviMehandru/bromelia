/// The switches every makemkvcon run gets: the profile file (nil: none), --minlength, --cache and --directio (nil: not
/// passed), more arguments (split like a POSIX command line), and whether to scan drives (false: --noscan).
public struct MakemkvOptions: Sendable, Equatable {
    public var profilePath: String?
    public var minLengthSeconds: Int?
    public var cacheMB: Int?
    public var directIO: Bool?
    public var extraArguments: String
    public var scan: Bool

    public init(profilePath: String? = nil, minLengthSeconds: Int? = nil, cacheMB: Int? = nil, directIO: Bool? = nil, extraArguments: String = "",
                scan: Bool = false) {
        self.profilePath = profilePath
        self.minLengthSeconds = minLengthSeconds
        self.cacheMB = cacheMB
        self.directIO = directIO
        self.extraArguments = extraArguments
        self.scan = scan
    }
}
