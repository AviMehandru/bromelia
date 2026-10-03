/// A track (stream) of a title: its kind (attribute 1), codec (6, else 5), language (3, 4), name (2), whether
/// it's the default (`d` in 38), and every attribute MakeMKV reported.
public struct Track: Sendable, Equatable {
    public var index: Int
    public var kind: TrackKind
    public var codec: String
    public var language: String
    public var languageName: String
    public var name: String
    public var isDefault: Bool
    public var attributes: [Int: String]

    public init(index: Int, kind: TrackKind, codec: String, language: String, languageName: String, name: String,
                isDefault: Bool, attributes: [Int: String]) {
        self.index = index
        self.kind = kind
        self.codec = codec
        self.language = language
        self.languageName = languageName
        self.name = name
        self.isDefault = isDefault
        self.attributes = attributes
    }
}
