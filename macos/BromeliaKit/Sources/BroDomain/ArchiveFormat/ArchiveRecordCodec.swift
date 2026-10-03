import BroFoundation

/// Reads archive records (versions 2 and 3) and writes version 3.
public enum ArchiveRecordCodec {
    /// The key order of each object of a version 3 record (archive-record-3.json); keys it doesn't name follow in the
    /// order they had.
    static let keyOrder: [String: [String]] = [
        "": ["format", "version", "unit", "status", "name", "kind", "work", "disc", "physical", "acquisition", "job", "titles", "episodes", "files",
             "attempts", "parity", "counts", "problems", "logs"],
        "unit": ["id", "short", "library"],
        "work": ["provider", "title", "year", "tmdbId", "imdbId", "chosen"],
        "disc": ["label", "volumeName", "type", "format", "formatCode", "encrypted", "season", "part", "volume", "disc", "set", "fingerprint", "libreDrive"],
        "physical": ["barcode", "location"],
        "acquisition": ["mode", "rip", "source", "drive", "profile", "rules", "makemkv", "bromelia", "tools"],
        "acquisition.drive": ["id", "name", "model"],
        "job": ["id", "startedAt", "finishedAt"],
        "titles[]": ["index", "sourceTitleId", "sourceFile", "duration", "durationSeconds", "chapters", "sizeBytes", "segmentMap", "readErrors"],
        "episodes[]": ["file", "episode", "sourceTitleId", "firstChapter", "lastChapter", "title"],
        "files[]": ["path", "size", "sha256", "role", "title"],
        "attempts[]": ["job", "drive", "startedAt", "titles", "kept"],
        "parity": ["percent", "files"],
        "counts": ["warnings", "errors"],
        "problems[]": ["code", "params", "text", "readError"],
    ]

    /// A record of version 2 or 3; nil when the bytes aren't one (not JSON, another format, another version).
    public static func decode(_ bytes: [UInt8]) -> ArchiveRecord? {
        guard let doc = JsonValue.parse(bytes), case .object = doc, doc["format"]?.string == "bromelia-archive",
              let version = doc["version"]?.int, version == 2 || version == 3 else { return nil }
        let disc = doc["disc"]
        func i(_ v: JsonValue?) -> Int? { v?.int.map { Int($0) } }
        let episodes = (doc["episodes"]?.array ?? []).compactMap { i($0["episode"]) }
        let files = (doc["files"]?.array ?? []).compactMap { f -> SumEntry? in
            guard let p = f["path"]?.string, let h = f["sha256"]?.string else { return nil }
            return SumEntry(path: p, sha256: h)
        }
        return ArchiveRecord(version: Int(version), document: doc, status: doc["status"]?.string ?? "", name: doc["name"]?.string ?? "",
                             kind: doc["kind"]?.string ?? "", label: disc?["label"]?.string ?? "", volumeName: disc?["volumeName"]?.string ?? "",
                             season: i(disc?["season"]), part: i(disc?["part"]), volume: i(disc?["volume"]), disc: i(disc?["disc"]),
                             fingerprint: disc?["fingerprint"]?.string, unitId: version == 3 ? doc["unit"]?["id"]?.string : nil,
                             episodes: episodes, files: files)
    }

    /// A version 3 record: canonical JSON with the keys in archive-record-3.json's order.
    public static func encodeV3(_ record: ArchiveRecord) -> [UInt8] {
        precondition(record.version == 3, "only version 3 records are written")
        return JsonValue.encodeCanonical(ordered(record.document, ""))
    }

    /// The record as an archived TV disc for EpisodeContinuation: status success or errors, kind tv; the label's title
    /// from the volume name (else the label); the highest episode number.
    public static func archivedDisc(_ record: ArchiveRecord, folder: String) -> ArchivedDisc? {
        guard record.status == "success" || record.status == "errors", record.kind == "tv" else { return nil }
        let label = LabelParser.parse(record.volumeName.isEmpty ? record.label : record.volumeName)
        return ArchivedDisc(name: record.name, labelTitle: label.title, season: record.season, part: record.part, volume: record.volume,
                            disc: record.disc, lastEpisode: record.episodes.max(), folder: folder)
    }

    static func ordered(_ v: JsonValue, _ path: String) -> JsonValue {
        switch v {
        case .array(let items):
            return .array(items.map { ordered($0, path + "[]") })
        case .object(let members):
            let mapped = members.map { (key: $0.key, value: ordered($0.value, (path.isEmpty ? "" : path + ".") + $0.key)) }
            guard let order = keyOrder[path] else { return .object(mapped) }
            let rank = { (k: String) in order.firstIndex(of: k) ?? order.count }
            // A stable sort: keys the schema doesn't name keep their order.
            let sorted = mapped.enumerated().sorted { a, b in
                rank(a.element.key) != rank(b.element.key) ? rank(a.element.key) < rank(b.element.key) : a.offset < b.offset
            }.map(\.element)
            return .object(sorted)
        default:
            return v
        }
    }
}
