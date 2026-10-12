/// A title of an opened disc as the user left it: ticked or not, the streams kept when they were picked by hand (nil:
/// MakeMKV's selection), and the output file name ("" = from the layout).
public struct TitleChoice: Sendable, Equatable {
    public var index: Int
    public var selected: Bool
    public var tracks: [Int]?
    public var name: String

    public init(index: Int, selected: Bool, tracks: [Int]? = nil, name: String = "") {
        self.index = index
        self.selected = selected
        self.tracks = tracks
        self.name = name
    }
}
