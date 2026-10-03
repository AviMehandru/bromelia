/// A track of an MKV as mkvmerge -J lists it: its id and type (video, audio, subtitles).
public struct MkvTrack: Sendable, Equatable {
    public var id: Int
    public var type: String

    public init(id: Int, type: String) {
        self.id = id
        self.type = type
    }
}
