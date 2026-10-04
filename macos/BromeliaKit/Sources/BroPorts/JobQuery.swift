import BroDomain
import BroFoundation

/// Which jobs: in a state, of a kind, finished after an instant; newest first, a page at a time.
public struct JobQuery: Sendable, Equatable {
    public var state: JobState?
    public var kind: JobKind?
    public var finishedAfter: Instant?
    public var limit: Int
    public var offset: Int

    public init(state: JobState? = nil, kind: JobKind? = nil, finishedAfter: Instant? = nil, limit: Int, offset: Int) {
        self.state = state
        self.kind = kind
        self.finishedAfter = finishedAfter
        self.limit = limit
        self.offset = offset
    }
}
