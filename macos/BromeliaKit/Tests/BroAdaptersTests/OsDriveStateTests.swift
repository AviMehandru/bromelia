import BroAdapters
import BroDomain
import BroFoundation
import BroPorts
import BroTestSupport
import Foundation
import Testing

/// A clock whose timers fire when the test says so.
final class ManualClock: Clock, @unchecked Sendable {
    final class Handle: TimerHandle, @unchecked Sendable {
        var cancelled = false
        func cancel() { cancelled = true }
    }

    private(set) var timers: [(schedule: TimerSchedule, handler: @Sendable () -> Void, handle: Handle)] = []
    func now() -> Instant { Instant(unixMilliseconds: 1_790_000_000_000) }
    func monotonic() -> Duration { Duration(seconds: 0) }
    func sleep(_ duration: Duration, cancel: CancellationToken) async throws(BroError) {}

    func timer(_ schedule: TimerSchedule, handler: @escaping @Sendable () -> Void) -> any TimerHandle {
        let h = Handle()
        timers.append((schedule, handler, h))
        return h
    }

    /// Fires every timer that isn't cancelled.
    func tick() { for t in timers where !t.handle.cancelled { t.handler() } }
}

final class Box<T>: @unchecked Sendable {
    var value: T
    init(_ value: T) { self.value = value }
}

/// shared/fixtures/adapters/os-drive-states.cases.json, and the drive list on this Mac.
struct OsDriveStateTests {
    func state(_ j: JsonValue) -> OsDriveState {
        OsDriveState(drive: OsDrive(device: j["device"]!.string!, identification: j["identification"]?.string ?? "", mountPath: j["mountPath"]?.string),
                   media: j["media"]?.bool == true)
    }

    func text(_ e: DeviceEvent) -> String {
        switch e {
        case let .driveAppeared(drive): return "driveAppeared \(drive.device) \(drive.identification)"
        case let .driveVanished(device): return "driveVanished \(device)"
        case let .mediaArrived(device): return "mediaArrived \(device)"
        case let .mediaRemoved(device): return "mediaRemoved \(device)"
        case let .trayOpened(device): return "trayOpened \(device)"
        case let .mounted(device, path): return "mounted \(device) \(path)"
        case let .unmounted(device): return "unmounted \(device)"
        }
    }

    @Test func theSharedCasesPass() throws {
        let doc = try Fixtures.json("adapters/os-drive-states.cases.json")
        var failures: [String] = []
        for c in doc["cases"]?.array ?? [] {
            let given = c["given"]!, expect = c["expect"]!
            do {
                if let snapshots = given["snapshots"]?.array {
                    let all = snapshots.map { $0.array!.map(state) }
                    let index = Box(0), got = Box<[String]>([])
                    let clock = ManualClock()
                    let poller = DrivePoller(snapshot: { all[index.value] }, clock: clock, interval: Duration(seconds: given["interval"]!.double!))
                    poller.start { e in got.value.append(text(e)) }
                    try Fixtures.same(1, clock.timers.count, "timers")
                    try Fixtures.same(TimerSchedule.every(interval: Duration(seconds: expect["timer"]!.double!)), clock.timers[0].schedule, "schedule")
                    for want in expect["afterTicks"]!.array! {
                        index.value += 1
                        got.value = []
                        clock.tick()
                        try Fixtures.same(want.array!.map { $0.string! }, got.value, "events")
                    }
                    try Fixtures.same(all[index.value].map(\.drive), poller.currentDrives(), "currentDrives")
                    poller.stop()
                    try Fixtures.check(clock.timers[0].handle.cancelled, "stopped")
                } else {
                    let events = OsDriveState.changes(given["before"]!.array!.map(state), after: given["after"]!.array!.map(state))
                    try Fixtures.same(expect["events"]!.array!.map { $0.string! }, events.map(text), "events")
                }
            } catch {
                failures.append("\(c["id"]!.string!): \(error)")
            }
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    /// The same number of drives as ioreg lists (none on a Mac without an optical drive).
    @Test func theDrivesAreWhatTheRegistryLists() throws {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/sbin/ioreg")
        p.arguments = ["-r", "-c", "IOCompactDiscServices", "-d", "1"]
        let pipe = Pipe()
        p.standardOutput = pipe
        try p.run()
        let out = String(decoding: pipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
        p.waitUntilExit()
        let listed = out.components(separatedBy: "\n").filter { $0.contains("+-o ") }.count
        let states = PlatformDeviceMonitor(clock: SystemClock()).snapshot()
        #expect(states.count == listed)
        for s in states { #expect(s.drive.device.hasPrefix("IOService:")) }
    }
}
