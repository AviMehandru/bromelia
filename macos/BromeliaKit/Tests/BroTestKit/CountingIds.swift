import BroFoundation
import BroPorts
import Foundation

/// Ids that count: 00000000-0000-4000-8000-000000000001, … after the `given` ones already used (moved from the
/// PipelineRunner harness).
public final class CountingIds: IdGenerator, @unchecked Sendable {
    private let lock = NSLock()
    private var n: Int64

    public init(_ given: Int64 = 0) { n = given }

    public static func id(_ n: Int64) -> Id {
        let hex = String(n, radix: 16)
        return Id("00000000-0000-4000-8000-" + String(repeating: "0", count: 12 - hex.count) + hex)
    }

    public func next() -> Id { lock.withLock { n += 1; return CountingIds.id(n) } }

    public func peek() -> Id { lock.withLock { CountingIds.id(n + 1) } }
}
