/// Whether the drive read the disc in LibreDrive mode, didn't, or the disc needs it.
public enum LibreDriveState: String, Sendable, CaseIterable {
    case enabled
    case notInUse
    case required
}
