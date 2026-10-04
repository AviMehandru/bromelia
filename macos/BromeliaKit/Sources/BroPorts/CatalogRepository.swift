import BroDomain
import BroFoundation

/// What has been archived.
public protocol CatalogRepository: Sendable {
    /// Committed units of the same disc.
    func archivedBefore(_ fingerprint: String) async throws(BroError) -> [UnitRecord]

    /// The disc before this one of the same set.
    func previousDisc(_ query: ContinuationQuery) async throws(BroError) -> ArchivedDisc?

    func highestEpisode(_ query: ContinuationQuery) async throws(BroError) -> Int?

    /// Works whose title contains query, ignoring case.
    func works(_ query: String) async throws(BroError) -> [WorkRecord]

    func set(_ setId: Id) async throws(BroError) -> DiscSetRecord?
}
