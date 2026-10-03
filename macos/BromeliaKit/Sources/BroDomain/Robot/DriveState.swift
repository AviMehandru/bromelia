/// A drive's state in a DRV line; MakeMKV's numbers are 0, 1, 2, 3, 256 and 257.
public enum DriveState: String, Sendable, CaseIterable {
    case emptyClosed
    case emptyOpen
    case inserted
    case loading
    case noDrive
    case unmounting
}
