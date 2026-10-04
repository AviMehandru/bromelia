import BroDomain
import BroFoundation
import BroPorts
import Foundation
import IOKit.pwr_mgt

/// PowerManager on macOS (plan §10.3): an IOKit assertion against idle sleep (IOPMAssertionCreateWithName) per guard;
/// releasing one leaves the others. power.unavailable when IOKit refuses.
public final class PlatformPowerManager: PowerManager {
    public init() {}

    public func inhibit(_ reason: String) throws(BroError) -> any PowerGuard {
        var id = IOPMAssertionID(0)
        let rc = IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleSystemSleep as CFString, IOPMAssertionLevel(kIOPMAssertionLevelOn),
                                             reason as CFString, &id)
        guard rc == kIOReturnSuccess else {
            throw BroMessage(.powerUnavailable, [("reason", .string(String(format: "IOKit error 0x%08x", rc)))], severity: .warning).toError()
        }
        return Guard(id)
    }

    private final class Guard: PowerGuard, @unchecked Sendable {
        private let lock = NSLock()
        private var id: IOPMAssertionID?
        init(_ id: IOPMAssertionID) { self.id = id }
        deinit { release() }

        func release() {
            let held: IOPMAssertionID? = lock.withLock {
                defer { id = nil }
                return id
            }
            if let held { IOPMAssertionRelease(held) }
        }
    }
}
