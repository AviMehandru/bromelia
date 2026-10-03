/// How a drive entry finds its drive: `driveName` (MakeMKV's identification, compared case- and
/// space-insensitively), else `devicePath`.
public struct DriveMatch: Sendable, Equatable {
    public var driveName: String
    public var devicePath: String

    public init(driveName: String = "", devicePath: String = "") {
        self.driveName = driveName
        self.devicePath = devicePath
    }
}
