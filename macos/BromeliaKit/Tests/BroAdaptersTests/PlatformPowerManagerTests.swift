import BroAdapters
import BroPorts
import Foundation
import IOKit.pwr_mgt
import Testing

/// platform-adapters.contract.json#power-guard on the real macOS.
struct PlatformPowerManagerTests {
    /// The names of this process's power assertions.
    func assertions() -> [String] {
        var dict: Unmanaged<CFDictionary>?
        guard IOPMCopyAssertionsByProcess(&dict) == kIOReturnSuccess, let all = dict?.takeRetainedValue() as? [NSNumber: [[String: Any]]] else { return [] }
        return (all[NSNumber(value: getpid())] ?? []).compactMap { $0[kIOPMAssertionNameKey] as? String }
    }

    @Test func eachGuardKeepsTheMacAwakeUntilItIsReleased() throws {
        let power = PlatformPowerManager()
        let first = try power.inhibit("Bromelia test: ripping")
        let second = try power.inhibit("Bromelia test: verifying")
        #expect(assertions().contains("Bromelia test: ripping"))
        #expect(assertions().contains("Bromelia test: verifying"))
        first.release()
        first.release()
        #expect(!assertions().contains("Bromelia test: ripping"))
        #expect(assertions().contains("Bromelia test: verifying"))
        second.release()
        #expect(!assertions().contains { $0.hasPrefix("Bromelia test") })
    }
}
