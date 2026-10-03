import BroFoundation

/// What `mkvmerge -J` says about a file: its duration (nil when it reports none), its tracks and the number of
/// chapters.
public struct MkvProbe: Sendable, Equatable {
    public var durationSeconds: Double?
    public var tracks: [MkvTrack]
    public var chapterCount: Int

    public init(durationSeconds: Double?, tracks: [MkvTrack], chapterCount: Int) {
        self.durationSeconds = durationSeconds
        self.tracks = tracks
        self.chapterCount = chapterCount
    }

    /// `mkvmerge -J`'s JSON; nil when it isn't JSON or mkvmerge didn't recognise the file.
    public static func parse(_ json: String) -> MkvProbe? {
        guard let root = JsonValue.parse(json), let container = root["container"], container.members != nil else { return nil }
        if container["recognized"]?.bool == false { return nil }
        let duration = container["properties"]?["duration"]?.double.map { $0 / 1e9 }
        let tracks = (root["tracks"]?.array ?? []).map { MkvTrack(id: Int($0["id"]?.int ?? 0), type: $0["type"]?.string ?? "") }
        let chapters = (root["chapters"]?.array ?? []).reduce(0) { $0 + Int($1["num_entries"]?.int ?? 0) }
        return MkvProbe(durationSeconds: duration, tracks: tracks, chapterCount: chapters)
    }
}
