import BroFoundation

/// An archive record (bromelia-<unit8>.json, version 3; today's bromelia.json, version 2): the document as read
/// (`document`, key order kept) and the fields Bromelia reads back: status, name, kind, the disc's label and place in
/// its set (nil: not in the record), its fingerprint, the unit's id (version 3), the episodes' numbers and the files
/// SHA256SUMS lists. Fields with the same meaning have the same path in both versions (archive-record-3.json).
public struct ArchiveRecord: Sendable, Equatable {
    public var version: Int
    public var document: JsonValue
    public var status: String
    public var name: String
    public var kind: String
    public var label: String
    public var volumeName: String
    public var season: Int?
    public var part: Int?
    public var volume: Int?
    public var disc: Int?
    public var fingerprint: String?
    public var unitId: String?
    public var episodes: [Int]
    public var files: [SumEntry]

    public init(version: Int, document: JsonValue, status: String, name: String, kind: String, label: String, volumeName: String,
                season: Int?, part: Int?, volume: Int?, disc: Int?, fingerprint: String?, unitId: String?, episodes: [Int], files: [SumEntry]) {
        self.version = version
        self.document = document
        self.status = status
        self.name = name
        self.kind = kind
        self.label = label
        self.volumeName = volumeName
        self.season = season
        self.part = part
        self.volume = volume
        self.disc = disc
        self.fingerprint = fingerprint
        self.unitId = unitId
        self.episodes = episodes
        self.files = files
    }
}
