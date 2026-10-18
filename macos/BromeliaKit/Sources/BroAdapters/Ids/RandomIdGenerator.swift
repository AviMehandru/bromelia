import BroFoundation
import BroPorts
import Foundation

/// Random (version 4) UUIDs in lower-case text.
public final class RandomIdGenerator: IdGenerator {
    public init() {}

    public func next() -> Id { Id(UUID().uuidString.lowercased()) }
}
