/// A track's kind (MakeMKV attribute 1).
public enum TrackKind: String, Sendable, CaseIterable {
    case video
    case audio
    case subtitle
    case attachment
    case unknown
}
