import BroDomain
import BroFoundation

/// archive_units, unit_files and the commit intents.
public protocol UnitRepository: Sendable {
    /// The unit (committing) and its intent, in one transaction.
    func beginCommit(_ unit: UnitRecord, intent: CommitIntent) async throws(BroError)

    /// Item seq of the intent has been moved.
    func markMoved(_ unitId: Id, seq: Int) async throws(BroError)

    /// The committed unit and its files; the intent goes.
    func finishCommit(_ intent: CommitIntent, record: UnitRecord, files: [UnitFile]) async throws(BroError)

    func openIntents() async throws(BroError) -> [CommitIntent]

    func get(_ unitId: Id) async throws(BroError) -> UnitRecord?

    func query(_ query: UnitQuery) async throws(BroError) -> [UnitRecord]

    /// Committed units, the ones checked longest ago first.
    func leastRecentlyVerified(_ limit: Int) async throws(BroError) -> [UnitRecord]

    func markMissing(_ unitId: Id) async throws(BroError)
}
