import BroDomain
import BroPorts
import Foundation

final class CountingSink: RunSink, @unchecked Sendable {
    private let lock = NSLock()
    private var n = 0
    var events: Int { lock.withLock { n } }
    func event(_ event: RobotEvent) { lock.withLock { n += 1 } }
}
