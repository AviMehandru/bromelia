import BroDomain
import BroFoundation

/// The outbox table.
public protocol OutboxRepository: Sendable {
    func add(_ notification: OutboxEntry) async throws(BroError)

    /// Unsent notifications whose next attempt is due.
    func due(_ now: Instant) async throws(BroError) -> [OutboxEntry]

    func markSent(_ id: Id) async throws(BroError)

    /// No retryAt: give up.
    func markFailed(_ id: Id, error: BroError, retryAt: Instant?) async throws(BroError)
}
