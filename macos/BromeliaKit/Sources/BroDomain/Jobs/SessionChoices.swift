import BroFoundation

/// What the user chose on an opened disc (dtos.json's SessionChoices): every title with whether it is ticked, the name
/// typed for the disc ("" = from the disc), movie or TV, year, online id, first episode, library and profile (nil =
/// the default).
public struct SessionChoices: Sendable, Equatable {
    public var titles: [TitleChoice]
    public var name: String
    public var kind: MediaKind?
    public var year: Int?
    public var onlineId: String
    public var firstEpisode: Int?
    public var library: String?
    public var profile: String?

    public init(titles: [TitleChoice], name: String = "", kind: MediaKind? = nil, year: Int? = nil, onlineId: String = "",
                firstEpisode: Int? = nil, library: String? = nil, profile: String? = nil) {
        self.titles = titles
        self.name = name
        self.kind = kind
        self.year = year
        self.onlineId = onlineId
        self.firstEpisode = firstEpisode
        self.library = library
        self.profile = profile
    }

    public static func encode(_ choices: SessionChoices) -> JsonValue {
        func n(_ v: Int?) -> JsonValue { v.map { .integer(Int64($0)) } ?? .null }
        func s(_ v: String?) -> JsonValue { v.map { .string($0) } ?? .null }
        return .object([
            ("titles", .array(choices.titles.map { t in
                var members: [(key: String, value: JsonValue)] = [("index", .integer(Int64(t.index))), ("selected", .bool(t.selected))]
                if let tracks = t.tracks { members.append(("tracks", .array(tracks.map { .integer(Int64($0)) }))) }
                if !t.name.isEmpty { members.append(("name", .string(t.name))) }
                return .object(members)
            })),
            ("name", .string(choices.name)), ("kind", s(choices.kind?.rawValue)), ("year", n(choices.year)),
            ("onlineId", .string(choices.onlineId)), ("firstEpisode", n(choices.firstEpisode)), ("library", s(choices.library)),
            ("profile", s(choices.profile)),
        ])
    }

    /// The choices `encode` wrote; what the JSON leaves out takes its default.
    public static func decode(_ json: JsonValue) -> SessionChoices {
        func n(_ v: JsonValue?) -> Int? { v?.int.map { Int($0) } }
        return SessionChoices(
            titles: (json["titles"]?.array ?? []).map { t in
                TitleChoice(index: n(t["index"]) ?? 0, selected: t["selected"]?.bool ?? false,
                            tracks: t["tracks"]?.array?.map { Int($0.int ?? 0) }, name: t["name"]?.string ?? "")
            },
            name: json["name"]?.string ?? "", kind: json["kind"]?.string.flatMap(MediaKind.init(rawValue:)), year: n(json["year"]),
            onlineId: json["onlineId"]?.string ?? "", firstEpisode: n(json["firstEpisode"]), library: json["library"]?.string,
            profile: json["profile"]?.string)
    }
}
