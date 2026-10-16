/// A drive's state as clients see it (dtos.json Drive.state): MakeMKV's state of a connected drive, or unavailable for a
/// configured drive that isn't connected.
public enum DriveStatus: String, Sendable, Equatable {
    case empty
    case trayOpen
    case loading
    case present
    case unavailable
    case unmounting
}
