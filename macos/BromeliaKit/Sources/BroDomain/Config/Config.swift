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
}
