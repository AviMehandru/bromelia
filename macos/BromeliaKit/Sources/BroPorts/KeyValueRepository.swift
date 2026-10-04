import BroDomain
import BroFoundation

/// The kv table: small JSON values.
public protocol KeyValueRepository: Sendable {
    func get(_ key: String) async throws(BroError) -> JsonValue?

    func set(_ key: String, value: JsonValue) async throws(BroError)
}
