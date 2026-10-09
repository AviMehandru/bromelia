/// What a makemkvcon run should leave in its destination: nothing (a listing), titles (MKV files of a rip), a backup (a
/// disc structure or an ISO image in a folder), or an image: a backup that came out as one file at the destination
/// itself, whatever it is called (MakeMKV writes DVD backups as ISO images even when a folder was asked for).
public enum RunProduct: String, Sendable, CaseIterable, Codable {
    case nothing
    case titles
    case backup
    case image
}
