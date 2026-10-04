/// replicas.state.
public enum ReplicaState: String, Sendable, CaseIterable {
    case pending
    case copying
    case verified
    case stale
    case failed
}
