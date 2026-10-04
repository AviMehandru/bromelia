/// A source makemkvcon can open: a drive (MakeMKV's index and the OS device, "" when unknown), a disc image, or a
/// folder holding a disc structure.
public enum MakemkvSource: Sendable, Equatable {
    case drive(index: Int, device: String)
    case iso(path: String)
    case file(path: String)
}
