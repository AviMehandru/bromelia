/// A title of a listing: MakeMKV's index (it changes with the minimum length setting), the source title (24) and
/// file (16), name (2), comment (49), duration (9), chapters (8), size (11), segment map (26), output file name
/// (27), angle (15), its tracks, and every attribute MakeMKV reported.
public struct Title: Sendable, Equatable {
    public var index: Int
    public var sourceTitleId: Int?
    public var sourceFile: String
    public var name: String
    public var comment: String
    public var duration: String
    public var durationSeconds: Int
    public var chapters: Int
    public var sizeBytes: Int64
    public var segmentMap: String
    public var outputFileName: String
    public var angle: Int?
    public var tracks: [Track]
    public var attributes: [Int: String]

    public init(index: Int, sourceTitleId: Int?, sourceFile: String, name: String, comment: String, duration: String,
                durationSeconds: Int, chapters: Int, sizeBytes: Int64, segmentMap: String, outputFileName: String, angle: Int?,
                tracks: [Track], attributes: [Int: String]) {
        self.index = index
        self.sourceTitleId = sourceTitleId
        self.sourceFile = sourceFile
        self.name = name
        self.comment = comment
        self.duration = duration
        self.durationSeconds = durationSeconds
        self.chapters = chapters
        self.sizeBytes = sizeBytes
        self.segmentMap = segmentMap
        self.outputFileName = outputFileName
        self.angle = angle
        self.tracks = tracks
        self.attributes = attributes
    }
}
