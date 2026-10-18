import BroFoundation

/// A unit that holds this disc already: its id, its folder (absolute) and the day it was committed (YYYY-MM-DD, UTC).
public struct ArchivedMatch: Sendable, Equatable {
    public var unitId: Id
    public var folder: String
    public var day: String

    public init(unitId: Id, folder: String, day: String) {
        self.unitId = unitId
        self.folder = folder
        self.day = day
    }

    public static func encode(_ match: ArchivedMatch) -> JsonValue {
        .object([("unitId", .string(match.unitId.value)), ("folder", .string(match.folder)), ("day", .string(match.day))])
    }

    public static func decode(_ json: JsonValue?) -> ArchivedMatch? {
        guard let json, json.members != nil else { return nil }
        return ArchivedMatch(unitId: Id(json["unitId"]?.string ?? ""), folder: json["folder"]?.string ?? "", day: json["day"]?.string ?? "")
    }
}
