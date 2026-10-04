import BroDomain
import BroFoundation

/// A row of replicas: a verified second copy.
public struct ReplicaRecord: Sendable, Equatable {
    public var id: Id
    public var unitId: Id
    public var targetId: String
    public var path: String
    public var state: ReplicaState
    public var verifiedAt: Instant?
    public var error: BroMessage?

    public init(id: Id, unitId: Id, targetId: String, path: String, state: ReplicaState, verifiedAt: Instant? = nil,
                error: BroMessage? = nil) {
        self.id = id
        self.unitId = unitId
        self.targetId = targetId
        self.path = path
        self.state = state
        self.verifiedAt = verifiedAt
        self.error = error
    }
}
