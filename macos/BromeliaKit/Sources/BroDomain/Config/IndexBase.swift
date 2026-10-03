/// What an index pattern counts: MakeMKV's title numbers or the source (playlist / VTS) ids.
public enum IndexBase: String, Sendable, CaseIterable {
    case makemkv
    case source
}
