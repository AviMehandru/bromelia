import BroFoundation

/// A rule of the configuration (config-3.json's Rule): when → then. Replaces plugins, formatModes and matchName /
/// matchFormats.
public struct Rule: Sendable, Equatable {
    public var id: String
    public var name: String
    public var enabled: Bool
    public var when: RuleWhen
    public var then: RuleThen

    public init(id: String, name: String, enabled: Bool, when: RuleWhen, then: RuleThen) {
        self.id = id
        self.name = name
        self.enabled = enabled
        self.when = when
        self.then = then
    }

    public static func decode(_ json: JsonValue) -> Rule {
        func strings(_ v: JsonValue?) -> [String]? { v?.array?.map { $0.string ?? "" } }
        let w = json["when"], t = json["then"]
        return Rule(id: json["id"]?.string ?? "", name: json["name"]?.string ?? "", enabled: json["enabled"]?.bool ?? true,
                    when: RuleWhen(nameOrLabel: w?["nameOrLabel"]?.string, name: w?["name"]?.string, label: w?["label"]?.string,
                                   formats: strings(w?["formats"]), kinds: strings(w?["kinds"]), drives: strings(w?["drives"]),
                                   profiles: strings(w?["profiles"]), automatic: w?["automatic"]?.bool),
                    then: RuleThen(profile: t?["profile"]?.string, set: t?["set"], steps: strings(t?["steps"]) ?? []))
    }
}
