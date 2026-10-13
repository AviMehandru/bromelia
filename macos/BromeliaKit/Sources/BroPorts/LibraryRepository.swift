import BroDomain
import BroFoundation

/// libraries: the libraries LibraryService has seen, with their markers.
public protocol LibraryRepository: Sendable {
    /// By id.
    func all() async throws(BroError) -> [LibraryRecord]

    func get(_ libraryId: String) async throws(BroError) -> LibraryRecord?

    /// Inserts the row or replaces every column.
    func upsert(_ library: LibraryRecord) async throws(BroError)

    /// Sets online; last_seen_at becomes `at` only when online. Nothing when there is no such row.
    func setSeen(_ libraryId: String, online: Bool, at: Instant) async throws(BroError)
}
