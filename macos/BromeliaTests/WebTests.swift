import Testing
import Foundation
@testable import Bromelia

private final class BundleToken {}

/// The web page's API without a network connection (loopback connections are blocked on some Macs).
@Suite("Web page API")
@MainActor
struct WebTests {
    init() {
        Paths.dataOverride = FileManager.default.temporaryDirectory.appendingPathComponent("bromelia-web-test", isDirectory: true)
    }

    private func model() throws -> (AppModel, String) {
        let model = AppModel(persistent: false)
        // No makemkvcon: a job fails at once instead of touching a drive.
        model.config.makemkvconPath = "/nonexistent/makemkvcon"
        let e = DriveScanEntry(index: 0, state: .inserted, flags: [.dvdFiles], driveName: "BD-RE TEST DRIVE", discName: "MOVIE_DISC", devicePath: "/dev/rdisk99")
        model.debugApplyScan([e])
        model.debugApplyScan([e])
        return (model, DriveItem.laneKey(for: e))
    }

    @Test func titlesSettingsAndLogs() throws {
        let (model, lane) = try model()
        #expect(model.webAction(kind: "drives", id: lane, action: "rip", query: ["titles": "1"]) == "Open the disc first")
        let url = try #require(Bundle(for: BundleToken.self).url(forResource: "info-dvd", withExtension: "txt"))
        let s = try #require(model.sessions[lane])
        s.info = DiscInfoBuilder.build(fromOutput: try String(contentsOf: url, encoding: .utf8))
        s.selectedTitles = [0]
        let drive = try #require((model.webStatus()["drives"] as? [[String: Any]])?.first)
        let opened = try #require(drive["opened"] as? [String: Any])
        let titles = try #require(opened["titles"] as? [[String: Any]])
        #expect(titles.count == s.info!.titles.count && (titles.first?["selected"] as? Bool) == true)
        #expect(model.webAction(kind: "drives", id: lane, action: "rip", query: ["titles": "999"]) == "No titles chosen")
        let chosen = s.info!.titles.map(\.index).suffix(2)
        #expect(model.webAction(kind: "drives", id: lane, action: "rip", query: ["titles": chosen.map(String.init).joined(separator: ",")]) == nil)
        let job = try #require(model.jobs.last)
        #expect(job.manualTitles == Array(chosen) && job.mode == .mkv)
        // The job failed at once (no makemkvcon); its log is in the history.
        #expect(model.webLog(id: job.id.uuidString) != nil)
        #expect(model.webLog(id: "nope") == nil)

        let settings = try #require(model.webStatus()["settings"] as? [String: Any])
        let configs = try #require(settings["drives"] as? [[String: Any]])
        let id = try #require(configs.first?["id"] as? String)
        #expect(model.webAction(kind: "settings", id: id, action: "set", query: ["autoRip": "1", "mode": "backup"]) == nil)
        #expect(model.config.defaultDrive.automation.autoRipOnInsert && model.config.defaultDrive.rip.mode == .backup)
        #expect(model.webAction(kind: "settings", id: id, action: "set", query: ["mode": "audioCD"]) == "Unknown mode “audioCD”")
        #expect(model.webAction(kind: "settings", id: UUID().uuidString, action: "set", query: [:]) == "No such drive configuration")
    }

    @Test func logTail() throws {
        let file = FileManager.default.temporaryDirectory.appendingPathComponent("bromelia-log-\(UUID().uuidString.prefix(6)).txt")
        defer { try? FileManager.default.removeItem(at: file) }
        try (1...1000).map { "line \($0)" }.joined(separator: "\n").appending("\n").write(to: file, atomically: true, encoding: .utf8)
        #expect(WebLog.tail(file.path).hasPrefix("line 1\n"))
        let tail = WebLog.tail(file.path, limit: 100)
        #expect(tail.hasPrefix("…\nline ") && tail.hasSuffix("line 1000\n") && tail.count < 110)
        #expect(WebLog.tail("/nonexistent") == "")
    }

    @Test func routes() throws {
        let (model, _) = try model()
        var c = WebUIConfig()
        c.enabled = true
        c.port = 51399
        model.web.apply(c)
        defer { model.web.stop() }
        guard model.web.running != nil else { return } // the port is taken
        func get(_ path: String) -> (Int, String) {
            let r = HTTPRequest.parse(Data("GET \(path) HTTP/1.1\r\nHost: localhost\r\n\r\n".utf8))!
            let (code, _, body) = model.web.handle(r)
            return (code, String(decoding: body, as: UTF8.self))
        }
        #expect(get("/api/jobs/nope/log").0 == 404)
        let status = get("/api/status")
        #expect(status.0 == 200 && status.1.contains("\"settings\""))
    }

    /// HTTPS: PEM files become an identity (the requests themselves run in CI, where connections to 127.0.0.1 work).
    @Test func tlsIdentity() throws {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("bromelia-tls-test-\(UUID().uuidString.prefix(6))")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let cert = dir.appendingPathComponent("cert.pem"), key = dir.appendingPathComponent("key.pem")
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/openssl")
        p.arguments = ["req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key.path, "-out", cert.path, "-days", "2", "-subj", "/CN=localhost"]
        p.standardError = FileHandle.nullDevice
        try p.run()
        p.waitUntilExit()
        try #require(p.terminationStatus == 0)
        _ = try WebTLS.identity(certificate: cert.path, key: key.path)
        #expect(throws: (any Error).self) { try WebTLS.identity(certificate: cert.path, key: cert.path) }
        var c = WebUIConfig()
        c.tlsCertificate = cert.path
        #expect(WebAccess.startProblem(c) == "HTTPS needs both the certificate and its key")
        c.tlsKey = key.path
        #expect(WebAccess.startProblem(c) == nil)
    }
}
