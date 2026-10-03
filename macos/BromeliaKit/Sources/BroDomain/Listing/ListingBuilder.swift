import BroFoundation

/// Builds a `Listing` from robot events (TCOUNT, CINFO, TINFO, SINFO; others are ignored).
public struct ListingBuilder: Sendable {
    private var reportedTitleCount = 0
    private var disc: [Int: String] = [:]
    private var titles: [Int: [Int: String]] = [:]
    private var tracks: [Int: [Int: [Int: String]]] = [:]

    public init() {}

    public mutating func feed(_ event: RobotEvent) {
        switch event {
        case .titleCount(let n):
            reportedTitleCount = n
        case let .discInfo(id, _, value):
            disc[id] = value
        case let .titleInfo(title, id, _, value):
            titles[title, default: [:]][id] = value
            if tracks[title] == nil { tracks[title] = [:] }
        case let .streamInfo(title, stream, id, _, value):
            if titles[title] == nil { titles[title] = [:] }
            tracks[title, default: [:]][stream, default: [:]][id] = value
        default:
            break
        }
    }

    public func build() -> Listing {
        let typeText = disc[AttributeId.type.rawValue] ?? ""
        return Listing(
            name: disc[AttributeId.name.rawValue] ?? disc[AttributeId.volumeName.rawValue] ?? "",
            volumeName: disc[AttributeId.volumeName.rawValue] ?? "",
            type: ListingBuilder.typeOf(typeText),
            typeText: typeText,
            reportedTitleCount: reportedTitleCount,
            titles: titles.keys.sorted().map { ListingBuilder.title($0, titles[$0] ?? [:], tracks[$0] ?? [:]) },
            attributes: disc)
    }

    private static func title(_ index: Int, _ a: [Int: String], _ tracks: [Int: [Int: String]]) -> Title {
        func get(_ id: AttributeId) -> String? { a[id.rawValue] }
        let duration = get(.duration) ?? ""
        return Title(
            index: index,
            sourceTitleId: get(.originalTitleId).flatMap(Robot.int),
            sourceFile: get(.sourceFileName) ?? "",
            name: get(.name) ?? "",
            comment: get(.comment) ?? "",
            duration: duration,
            durationSeconds: Int(Duration.parseClock(duration)?.seconds ?? 0),
            chapters: get(.chapterCount).flatMap(Robot.int) ?? 0,
            sizeBytes: get(.diskSizeBytes).flatMap(int64) ?? 0,
            segmentMap: get(.segmentsMap) ?? "",
            outputFileName: get(.outputFileName) ?? "",
            angle: get(.angleInfo).flatMap(Robot.int),
            tracks: tracks.keys.sorted().map { track($0, tracks[$0] ?? [:]) },
            attributes: a)
    }

    private static func track(_ index: Int, _ a: [Int: String]) -> Track {
        func get(_ id: AttributeId) -> String? { a[id.rawValue] }
        return Track(
            index: index,
            kind: kindOf(get(.type)),
            codec: get(.codecShort) ?? get(.codecId) ?? "",
            language: get(.langCode) ?? "",
            languageName: get(.langName) ?? "",
            name: get(.name) ?? "",
            isDefault: (get(.mkvFlags) ?? "").contains("d"),
            attributes: a)
    }

    private static func int64(_ s: String) -> Int64? {
        let t = String(s.drop { $0 == " " || $0 == "\t" }.reversed().drop { $0 == " " || $0 == "\t" }.reversed())
        let digits = t.hasPrefix("-") || t.hasPrefix("+") ? t.dropFirst() : Substring(t)
        guard !digits.isEmpty, digits.allSatisfy({ $0.isASCII && $0.isNumber }) else { return nil }
        return Int64(t.hasPrefix("+") ? String(digits) : t)
    }

    private static func kindOf(_ type: String?) -> TrackKind {
        guard let type else { return .unknown }
        switch MessageCatalog.asciiLower(type) {
        case "video": return .video
        case "audio": return .audio
        case "subtitles", "subtitle": return .subtitle
        case "attachment": return .attachment
        default: return .unknown
        }
    }

    private static func typeOf(_ typeText: String) -> DiscType {
        let t = MessageCatalog.asciiLower(typeText)
        if t.contains("blu") { return .bd }
        if t.contains("hd") { return .hddvd }
        if t.contains("dvd") { return .dvd }
        return .disc
    }
}
