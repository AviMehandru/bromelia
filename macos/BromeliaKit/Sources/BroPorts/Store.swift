import BroDomain
import BroFoundation

/// Repositories over shared/schema/db; every write goes through one writer.
public protocol Store: Sendable {
    func jobs() -> any JobRepository

    func steps() -> any StepRepository

    func units() -> any UnitRepository

    func catalog() -> any CatalogRepository

    func checks() -> any CheckRepository

    func replicas() -> any ReplicaRepository

    func drives() -> any DriveRepository

    func lookupCache() -> any LookupCacheRepository

    func outbox() -> any OutboxRepository

    func kv() -> any KeyValueRepository

    /// Runs block in one transaction; a failure rolls it back.
    func transaction<T: Sendable>(_ block: @Sendable (any Store) async throws(BroError) -> T) async throws(BroError) -> T

    /// Applies the migrations the database lacks (shared/schema/db).
    func migrate() async throws(BroError)
}
