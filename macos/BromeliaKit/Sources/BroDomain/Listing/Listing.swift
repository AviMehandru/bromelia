import BroFoundation

/// What makemkvcon reported about a disc, ISO or folder: its name (2, else 32), volume name (32), type (from 1),
/// the number of titles it announced, the titles in index order, and every disc attribute.
public struct Listing: Sendable, Equatable {
    public var name: String
    public var volumeName: String
    public var type: DiscType
    public var typeText: String
    public var reportedTitleCount: Int
    public var titles: [Title]
    public var attributes: [Int: String]

    public init(name: String, volumeName: String, type: DiscType, typeText: String, reportedTitleCount: Int, titles: [Title],
                attributes: [Int: String]) {
        self.name = name
        self.volumeName = volumeName
        self.type = type
        self.typeText = typeText
        self.reportedTitleCount = reportedTitleCount
        self.titles = titles
        self.attributes = attributes
    }

    public static let empty = Listing(name: "", volumeName: "", type: .disc, typeText: "", reportedTitleCount: 0, titles: [], attributes: [:])
}

extension Listing {
    /// The listing as JSON: dtos.json's Listing, as shared/fixtures/domain/info-dvd.listing.expected.json writes it
    /// (attributes keyed by their id as text, in id order).
    public static func encode(_ listing: Listing) -> JsonValue {
        .object([
            ("name", .string(listing.name)), ("volumeName", .string(listing.volumeName)), ("type", .string(listing.type.rawValue)),
            ("typeText", .string(listing.typeText)), ("reportedTitleCount", .integer(Int64(listing.reportedTitleCount))),
            ("titles", .array(listing.titles.map { t in
                .object([
                    ("index", .integer(Int64(t.index))), ("sourceTitleId", orNull(t.sourceTitleId)), ("sourceFile", .string(t.sourceFile)),
                    ("name", .string(t.name)), ("comment", .string(t.comment)), ("duration", .string(t.duration)),
                    ("durationSeconds", .integer(Int64(t.durationSeconds))), ("chapters", .integer(Int64(t.chapters))),
                    ("sizeBytes", .integer(t.sizeBytes)), ("segmentMap", .string(t.segmentMap)), ("outputFileName", .string(t.outputFileName)),
                    ("angle", orNull(t.angle)),
                    ("tracks", .array(t.tracks.map { k in
                        .object([("index", .integer(Int64(k.index))), ("kind", .string(k.kind.rawValue)), ("codec", .string(k.codec)),
                                 ("language", .string(k.language)), ("languageName", .string(k.languageName)), ("name", .string(k.name)),
                                 ("isDefault", .bool(k.isDefault)), ("attributes", attributesJson(k.attributes))])
                    })),
                    ("attributes", attributesJson(t.attributes)),
                ])
            })),
            ("attributes", attributesJson(listing.attributes)),
        ])
    }

    /// A listing from its JSON (`encode`); what the JSON leaves out is empty.
    public static func decode(_ json: JsonValue) -> Listing {
        func int(_ v: JsonValue?) -> Int { Int(v?.int ?? 0) }
        return Listing(
            name: json["name"]?.string ?? "", volumeName: json["volumeName"]?.string ?? "",
            type: json["type"]?.string.flatMap(DiscType.init(rawValue:)) ?? .disc, typeText: json["typeText"]?.string ?? "",
            reportedTitleCount: int(json["reportedTitleCount"]),
            titles: (json["titles"]?.array ?? []).map { t in
                Title(index: int(t["index"]), sourceTitleId: t["sourceTitleId"]?.int.map { Int($0) }, sourceFile: t["sourceFile"]?.string ?? "",
                      name: t["name"]?.string ?? "", comment: t["comment"]?.string ?? "", duration: t["duration"]?.string ?? "",
                      durationSeconds: int(t["durationSeconds"]), chapters: int(t["chapters"]), sizeBytes: t["sizeBytes"]?.int ?? 0,
                      segmentMap: t["segmentMap"]?.string ?? "", outputFileName: t["outputFileName"]?.string ?? "",
                      angle: t["angle"]?.int.map { Int($0) },
                      tracks: (t["tracks"]?.array ?? []).map { k in
                          Track(index: int(k["index"]), kind: k["kind"]?.string.flatMap(TrackKind.init(rawValue:)) ?? .unknown,
                                codec: k["codec"]?.string ?? "", language: k["language"]?.string ?? "",
                                languageName: k["languageName"]?.string ?? "", name: k["name"]?.string ?? "",
                                isDefault: k["isDefault"]?.bool ?? false, attributes: attributesOf(k["attributes"]))
                      },
                      attributes: attributesOf(t["attributes"]))
            },
            attributes: attributesOf(json["attributes"]))
    }

    private static func orNull(_ v: Int?) -> JsonValue { v.map { .integer(Int64($0)) } ?? .null }

    private static func attributesJson(_ a: [Int: String]) -> JsonValue {
        .object(a.keys.sorted().map { (String($0), .string(a[$0] ?? "")) })
    }

    private static func attributesOf(_ json: JsonValue?) -> [Int: String] {
        var result: [Int: String] = [:]
        for (key, value) in json?.members ?? [] {
            if !key.isEmpty, key.utf8.allSatisfy({ $0 >= 48 && $0 <= 57 }), let id = Int32(key), let s = value.string { result[Int(id)] = s }
        }
        return result
    }
}
