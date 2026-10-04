import BroDomain
import BroFoundation

/// The replicas table.
public protocol ReplicaRepository: Sendable {
    func upsert(_ replica: ReplicaRecord) async throws(BroError)

    func forUnit(_ unitId: Id) async throws(BroError) -> [ReplicaRecord]

    /// Replicas that aren't verified.
    func lagging() async throws(BroError) -> [ReplicaRecord]
}
