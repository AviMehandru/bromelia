/// What `TokenRegistry.values` needs besides the identity: the rip word (Rip / Backup), the drive's name, the disc and
/// volume names, the disc type, the job's short id, the release year (from the online lookup) and the local time.
public struct TokenContext: Sendable, Equatable {
    public var rip: String
    public var drive: String
    public var disc: String
    public var volume: String
    public var type: DiscType
    public var job: String
    public var releaseYear: Int?
    public var localTime: LocalTime?

    public init(rip: String, drive: String = "", disc: String = "", volume: String = "", type: DiscType = .disc, job: String = "",
                releaseYear: Int? = nil, localTime: LocalTime? = nil) {
        self.rip = rip
        self.drive = drive
        self.disc = disc
        self.volume = volume
        self.type = type
        self.job = job
        self.releaseYear = releaseYear
        self.localTime = localTime
    }
}
