import BroDomain
import BroFoundation

/// What PlanStep decided, for the steps after it: the mode, the profile (its id and every field, so a resumed job keeps
/// it), the library and whether it was offline, the titles to rip (numbered as in the listing Probe read; with the tracks
/// and file name chosen by hand), the minimum length of a one-pass rip, the bytes the titles need (before the margin;
/// nil: no check) and whether they are reserved, the archived unit Decide asks about, and the identity under the mode.
public struct PlanOutput: Sendable, Equatable {
    public var mode: RipMode
    public var profileId: String
    public var profile: JsonValue
    public var libraryId: String
    public var libraryOffline: Bool
    public var titles: [TitleChoice]
    public var onePassMinLength: Int?
    public var spaceNeed: Int64?
    public var spaceReserved: Bool
    public var askArchived: ArchivedMatch?
    public var identity: Identity

    public init(mode: RipMode, profileId: String, profile: JsonValue, libraryId: String, libraryOffline: Bool, titles: [TitleChoice],
                onePassMinLength: Int?, spaceNeed: Int64?, spaceReserved: Bool, askArchived: ArchivedMatch?, identity: Identity) {
        self.mode = mode
        self.profileId = profileId
        self.profile = profile
        self.libraryId = libraryId
        self.libraryOffline = libraryOffline
        self.titles = titles
        self.onePassMinLength = onePassMinLength
        self.spaceNeed = spaceNeed
        self.spaceReserved = spaceReserved
        self.askArchived = askArchived
        self.identity = identity
    }

    public static func encode(_ plan: PlanOutput) -> JsonValue {
        .object([
            ("mode", .string(plan.mode.rawValue)), ("profileId", .string(plan.profileId)), ("profile", plan.profile),
            ("libraryId", .string(plan.libraryId)), ("libraryOffline", .bool(plan.libraryOffline)),
            ("titles", .array(plan.titles.map { t in
                var members: [(key: String, value: JsonValue)] = [("index", .integer(Int64(t.index)))]
                if let tracks = t.tracks { members.append(("tracks", .array(tracks.map { .integer(Int64($0)) }))) }
                if !t.name.isEmpty { members.append(("name", .string(t.name))) }
                return .object(members)
            })),
            ("onePass", plan.onePassMinLength.map { .integer(Int64($0)) } ?? .null),
            ("spaceNeed", plan.spaceNeed.map { .integer($0) } ?? .null), ("spaceReserved", .bool(plan.spaceReserved)),
            ("askArchived", plan.askArchived.map(ArchivedMatch.encode) ?? .null), ("identity", Identity.encode(plan.identity)),
        ])
    }

    /// The plan in the job's Plan output; nil when Plan hasn't run.
    public static func of(_ job: JobSnapshot) -> PlanOutput? {
        guard let o = JobSnapshot.output(job, .plan), o.members != nil else { return nil }
        return decode(o)
    }

    public static func decode(_ json: JsonValue) -> PlanOutput {
        PlanOutput(
            mode: json["mode"]?.string.flatMap(RipMode.init(rawValue:)) ?? .mkv, profileId: json["profileId"]?.string ?? "",
            profile: json["profile"] ?? .object([]), libraryId: json["libraryId"]?.string ?? "", libraryOffline: json["libraryOffline"]?.bool ?? false,
            titles: (json["titles"]?.array ?? []).map { t in
                TitleChoice(index: Int(t["index"]?.int ?? 0), selected: true, tracks: t["tracks"]?.array?.map { Int($0.int ?? 0) }, name: t["name"]?.string ?? "")
            },
            onePassMinLength: json["onePass"]?.int.map { Int($0) }, spaceNeed: json["spaceNeed"]?.int, spaceReserved: json["spaceReserved"]?.bool ?? false,
            askArchived: ArchivedMatch.decode(json["askArchived"]), identity: Identity.decode(json["identity"] ?? .object([])))
    }
}
