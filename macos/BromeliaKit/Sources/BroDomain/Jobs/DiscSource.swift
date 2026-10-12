import BroFoundation

/// Where a video-disc job reads (dtos.json's SessionSource): a drive, a disc image, a folder holding a disc structure,
/// or a file inside one (SourceResolver opens the disc it belongs to).
public enum DiscSource: Sendable, Equatable {
    case drive(driveId: String)
    case iso(path: String)
    case folder(path: String)
    case file(path: String)

    /// `{"drive": id}`, `{"iso": path}`, `{"folder": path}` or `{"file": path}`.
    public static func encode(_ source: DiscSource) -> JsonValue {
        switch source {
        case .drive(let id): return .object([("drive", .string(id))])
        case .iso(let p): return .object([("iso", .string(p))])
        case .folder(let p): return .object([("folder", .string(p))])
        case .file(let p): return .object([("file", .string(p))])
        }
    }

    /// The source `encode` wrote; nil for anything else.
    public static func decode(_ json: JsonValue) -> DiscSource? {
        if let d = json["drive"]?.string { return .drive(driveId: d) }
        if let i = json["iso"]?.string { return .iso(path: i) }
        if let f = json["folder"]?.string { return .folder(path: f) }
        if let f = json["file"]?.string { return .file(path: f) }
        return nil
    }
}
