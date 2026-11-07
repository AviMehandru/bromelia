import Testing
import Foundation
@testable import Bromelia

@Suite("Headless")
struct HeadlessTests {
    @Test func options() throws {
        let o = try Headless.parse(["--headless", "--config", "~/b.json", "-l", "0.0.0.0", "--port", "8080", "--token", "t",
                                    "-NSDocumentRevisionsDebugMode", "YES"]).get()
        #expect(o == Headless.Options(config: "~/b.json", listen: "0.0.0.0", port: 8080, token: "t"))
        #expect((try? Headless.parse(["--headless", "--port", "0"]).get()) == nil)
        #expect((try? Headless.parse(["--headless", "--bogus"]).get()) == nil)
        #expect((try? Headless.parse(["--headless", "--config"]).get()) == nil)
        #expect(Headless.command([]) == nil && Headless.command(["-NSDocumentRevisionsDebugMode", "YES"]) == nil)
    }

    @Test func launchAgent() {
        let p = LaunchAgent.plist(executable: "/Applications/Bromelia.app/Contents/MacOS/Bromelia", arguments: ["--port", "8080"])
        #expect(p["Label"] as? String == "app.bromelia.headless")
        #expect(p["ProgramArguments"] as? [String] == ["/Applications/Bromelia.app/Contents/MacOS/Bromelia", "--headless", "--port", "8080"])
        #expect(p["RunAtLoad"] as? Bool == true)
        #expect((p["KeepAlive"] as? [String: Bool])?["SuccessfulExit"] == false)
    }

    /// One holder at a time; the next one takes over once the first lets go.
    @Test func automationLock() {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("bromelia-lock-\(UUID().uuidString.prefix(6))/automation.lock")
        defer { try? FileManager.default.removeItem(at: url.deletingLastPathComponent()) }
        var first: AutomationLock? = AutomationLock(url: url)
        let second = AutomationLock(url: url)
        #expect(first!.acquire(who: "the app"))
        #expect(first!.acquire(who: "the app"), "again: still held")
        #expect(!second.acquire(who: "headless"))
        #expect(second.holder == "the app \(getpid())")
        first = nil
        // A child another test is spawning holds a copy of the descriptor until its exec, and a flock lasts until the last
        // copy closes, so the next holder may have to try again, as it does every minute in the app.
        var taken = false
        for _ in 0..<300 where !taken {
            taken = second.acquire(who: "headless")
            if !taken { usleep(10_000) }
        }
        #expect(taken && second.holder.hasPrefix("headless "))
    }
}
