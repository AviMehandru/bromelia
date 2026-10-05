/// What a makemkvcon run should leave in its destination: nothing (a listing), titles (MKV files of a rip) or a backup
/// (a disc structure or an ISO image).
public enum RunProduct: String, Sendable, CaseIterable, Codable {
    case nothing
    case titles
    case backup
}
