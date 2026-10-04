import BroDomain
import BroFoundation

/// Which units: of a library, matching a name, in a state; a page at a time.
public struct UnitQuery: Sendable, Equatable {
    public var libraryId: String?
    public var text: String?
    public var state: UnitState?
    public var limit: Int
    public var offset: Int

    public init(libraryId: String? = nil, text: String? = nil, state: UnitState? = nil, limit: Int, offset: Int) {
        self.libraryId = libraryId
        self.text = text
        self.state = state
        self.limit = limit
        self.offset = offset
    }
}
