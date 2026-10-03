import BroFoundation

/// A drive of the configuration (config-3.json's `Drive`): its profile (nil = the default profile), MakeMKV
/// settings of its own and automation (nil = automation.defaults).
public struct DriveEntry: Sendable, Equatable {
    public var id: String
    public var name: String
    public var enabled: Bool
    public var match: DriveMatch
    public var profile: String?
    public var makemkvSettings: [String: String]
    public var automation: JsonValue?

    public init(id: String, match: DriveMatch, name: String = "Drive", enabled: Bool = true, profile: String? = nil,
                makemkvSettings: [String: String] = [:], automation: JsonValue? = nil) {
        self.id = id
        self.name = name
        self.enabled = enabled
        self.match = match
        self.profile = profile
        self.makemkvSettings = makemkvSettings
        self.automation = automation
    }

    /// A drive entry from its JSON, with the schema's defaults for what it leaves out.
    public static func decode(_ json: JsonValue) -> DriveEntry {
        let m = json["match"]
        let automation = json["automation"].flatMap { $0.isNull ? nil : $0 }
        return DriveEntry(id: json["id"]?.string ?? "", match: DriveMatch(driveName: m?["driveName"]?.string ?? "", devicePath: m?["devicePath"]?.string ?? ""),
                          name: json["name"]?.string ?? "Drive", enabled: json["enabled"]?.bool ?? true, profile: json["profile"]?.string,
                          makemkvSettings: Dictionary((json["makemkvSettings"]?.members ?? []).map { ($0.key, $0.value.string ?? "") }, uniquingKeysWith: { $1 }),
                          automation: automation)
    }
}
