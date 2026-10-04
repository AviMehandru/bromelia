import BroDomain
import BroFoundation

/// Online lookup answers (lookup_cache).
public protocol LookupCacheRepository: Sendable {
    /// The answer, while it hasn't expired.
    func get(_ provider: String, key: String) async throws(BroError) -> HttpResponse?

    /// key: the request without credentials.
    func put(_ provider: String, key: String, response: HttpResponse, expiresAt: Instant) async throws(BroError)
}
