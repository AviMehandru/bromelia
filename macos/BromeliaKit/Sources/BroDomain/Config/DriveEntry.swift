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
}
