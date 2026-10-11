import BroFoundation
import BroPorts

/// Read-only questions about the archive (plan §12.2), over the catalogue.
public protocol CatalogQueries: Sendable {
    /// Committed units of a disc with this fingerprint.
    func archivedBefore(_ fingerprint: String) async throws(BroError) -> [UnitRecord]
}
