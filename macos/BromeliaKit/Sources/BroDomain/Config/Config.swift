import BroFoundation

/// A configuration, version 3 (shared/schema/config-3.json): the document as decoded (keys in schema order, defaults
/// filled outside profiles, unknown keys dropped) and the issues decoding found. Typed views of its entries:
/// `profiles`, `drives`, `steps`, `rules`.
public struct Config: Sendable, Equatable {
    public var document: JsonValue
    public var issues: [Issue]

    public init(document: JsonValue, issues: [Issue]) {
        self.document = document
        self.issues = issues
    }

    public static func profiles(_ config: Config) -> [Profile] { (config.document["profiles"]?.array ?? []).map(Profile.init) }
    public static func drives(_ config: Config) -> [DriveEntry] { (config.document["drives"]?.array ?? []).map(DriveEntry.decode) }
    public static func steps(_ config: Config) -> [StepDefinition] { (config.document["steps"]?.array ?? []).map(StepDefinition.decode) }
    public static func rules(_ config: Config) -> [Rule] { (config.document["rules"]?.array ?? []).map(Rule.decode) }

    /// The automation of a drive entry (config-3.json's DriveAutomation, every field set): the entry's own, else
    /// automation.defaults (also for no entry, or an id no entry has).
    public static func automation(_ config: Config, _ entryId: String?) -> JsonValue {
        let own = entryId.flatMap { id in (config.document["drives"]?.array ?? []).first { $0["id"]?.string == id }?["automation"] }
        var chosen: JsonValue = .object([])
        if let own, own.members != nil { chosen = own } else if let d = config.document["automation"]?["defaults"] { chosen = d }
        var issues: [Issue] = []
        return SchemaWalker.normalize(chosen, SchemaWalker.def("DriveAutomation"), "", fill: true, full: true, &issues)
    }
}
