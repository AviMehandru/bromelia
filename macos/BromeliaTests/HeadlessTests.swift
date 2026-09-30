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
        #expect(second.acquire(who: "headless") && second.holder.hasPrefix("headless "))
    }
}
