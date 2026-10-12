import BroDomain
import BroFoundation

/// The configuration as clients get it (dtos.json ConfigEnvelope): the saved revision, the document, its issues, and
/// every secret it refers to with whether it is set.
public struct ConfigEnvelope: Sendable, Equatable {
    public var version: Int64
    public var config: Config
    public var issues: [Issue]
    public var secrets: [SecretStatus]

    public init(version: Int64, config: Config, issues: [Issue], secrets: [SecretStatus]) {
        self.version = version
        self.config = config
        self.issues = issues
        self.secrets = secrets
    }

    public static func json(_ envelope: ConfigEnvelope) -> JsonValue {
        .object([("version", .integer(envelope.version)), ("doc", envelope.config.document),
                 ("issues", .array(envelope.issues.map { $0.toJson() })),
                 ("secrets", .array(envelope.secrets.map { .object([("name", .string($0.name)), ("set", .bool($0.set))]) }))])
    }
}
