import BroDomain
import BroFoundation

/// Secrets (config SecretRef names), outside the configuration.
public protocol Keystore: Sendable {
    /// None when there is no such secret.
    func get(_ name: String) throws(BroError) -> String?

    func set(_ name: String, value: String) throws(BroError)

    func remove(_ name: String) throws(BroError)
}
