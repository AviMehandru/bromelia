import Foundation
import AppKit

/// Starts the app, or Bromelia without a window when the command line asks for it (see Headless).
@main
enum Main {
    static func main() {
        let args = Array(CommandLine.arguments.dropFirst())
        if let code = Headless.command(args) { exit(code) }
        BromeliaApp.main()
    }
}

/// Only one Bromelia process at a time rips inserted discs and runs the scheduled archive check: the one that holds
/// `automation.lock` in the data folder (the app, or Bromelia running in the background). The other one checks again
/// every minute, so it takes over when the first one quits.
final class AutomationLock {
    private var fd: Int32 = -1
    let url: URL

    init(url: URL = Paths.appSupport.appendingPathComponent("automation.lock")) { self.url = url }

    deinit { if fd >= 0 { close(fd) } }

    var isHeld: Bool { fd >= 0 }

    /// Takes the lock unless another process has it; `who` ("the app", "headless") is written into the file.
    @discardableResult
    func acquire(who: String) -> Bool {
        if fd >= 0 { return true }
        try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        let f = open(url.path, O_RDWR | O_CREAT, 0o644)
        guard f >= 0 else { return false }
        guard flock(f, LOCK_EX | LOCK_NB) == 0 else { close(f); return false }
        fd = f
        let text = "\(who) \(getpid())\n"
        ftruncate(f, 0)
        _ = text.withCString { write(f, $0, strlen($0)) }
        return true
    }

    /// Who holds the lock, as written into the file ("headless 1234"), or "".
    var holder: String { ((try? String(contentsOf: url, encoding: .utf8)) ?? "").trimmingCharacters(in: .whitespacesAndNewlines) }
}

/// The login item that runs Bromelia in the background (`Bromelia --headless`), through launchd.
enum LaunchAgent {
    static let label = "app.bromelia.headless"

    static var plistURL: URL {
        FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/LaunchAgents/\(label).plist")
    }

    static var logURL: URL {
        FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Logs/Bromelia/headless.log")
    }

    static var isInstalled: Bool { FileManager.default.fileExists(atPath: plistURL.path) }

    /// The job: started at login and again if it stops with an error, output in ~/Library/Logs/Bromelia.
    static func plist(executable: String, arguments: [String]) -> [String: Any] {
        ["Label": label, "ProgramArguments": [executable, "--headless"] + arguments, "RunAtLoad": true,
         "KeepAlive": ["SuccessfulExit": false], "StandardOutPath": logURL.path, "StandardErrorPath": logURL.path,
         "ProcessType": "Standard"]
    }

    private static func launchctl(_ args: [String]) -> Int32 {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/launchctl")
        p.arguments = args
        p.standardOutput = FileHandle.nullDevice
        p.standardError = FileHandle.nullDevice
        guard (try? p.run()) != nil else { return -1 }
        p.waitUntilExit()
        return p.terminationStatus
    }

    /// Writes the job and starts it now. Returns an error, or nil.
    static func install(arguments: [String] = []) -> String? {
        guard let exe = Bundle.main.executableURL?.path else { return "Can't find Bromelia's program" }
        do {
            try FileManager.default.createDirectory(at: plistURL.deletingLastPathComponent(), withIntermediateDirectories: true)
            try FileManager.default.createDirectory(at: logURL.deletingLastPathComponent(), withIntermediateDirectories: true)
            let data = try PropertyListSerialization.data(fromPropertyList: plist(executable: exe, arguments: arguments), format: .xml, options: 0)
            try data.write(to: plistURL)
        } catch {
            return "Can't write \(plistURL.path): \(error.localizedDescription)"
        }
        _ = launchctl(["bootout", "gui/\(getuid())/\(label)"])
        let status = launchctl(["bootstrap", "gui/\(getuid())", plistURL.path])
        return status == 0 ? nil : "launchctl couldn't start it (status \(status)); it starts at the next login"
    }

    /// Stops the job and removes it.
    static func uninstall() -> String? {
        _ = launchctl(["bootout", "gui/\(getuid())/\(label)"])
        do {
            if isInstalled { try FileManager.default.removeItem(at: plistURL) }
            return nil
        } catch {
            return "Can't remove \(plistURL.path): \(error.localizedDescription)"
        }
    }
}

/// Bromelia without a window, like bromelia-daemon on Linux: watches the drives, rips discs as the configuration says,
/// runs post-processing and the scheduled archive check, and serves the web page. One line per job change on stdout.
enum Headless {
    struct Options: Equatable {
        var config: String?
        var listen: String?
        var port: Int?
        var token: String?
    }

    static let usage = """
        Usage: Bromelia --headless [--config PATH] [--listen ADDRESS] [--port PORT] [--token TOKEN]
               Bromelia --install-agent [options]   run it in the background at every login (launchd)
               Bromelia --uninstall-agent
        Rips discs without a window, as the configuration says (the app writes it; see docs/configuration.md).
          --config PATH      configuration file (default: ~/Library/Application Support/Bromelia/config.json)
          --listen ADDRESS   serve the web page on this address (0.0.0.0 = the network; needs a token)
          --port PORT        port of the web page (default 51280)
          --token TOKEN      token for the web page (or BROMELIA_WEB_TOKEN)
        """

    /// Options after --headless / --install-agent, or an error.
    static func parse(_ args: [String]) -> Result<Options, JobError> {
        var o = Options()
        var i = 0
        func value() -> String? { i + 1 < args.count ? args[i + 1] : nil }
        while i < args.count {
            let a = args[i]
            switch a {
            case "--headless", "--install-agent":
                break
            case "--config", "-c", "--listen", "-l", "--port", "-p", "--token", "-t":
                guard let v = value() else { return .failure(.message("\(a) needs a value")) }
                switch a {
                case "--config", "-c": o.config = v
                case "--listen", "-l": o.listen = v
                case "--port", "-p":
                    guard let p = Int(v), (1...65535).contains(p) else { return .failure(.message("Port \(v) is not valid")) }
                    o.port = p
                default: o.token = v
                }
                i += 1
            default:
                // Xcode and the system add their own: -NSDocumentRevisionsDebugMode YES, -psn_0_1234.
                if a.hasPrefix("-NS") || a.hasPrefix("-Apple") {
                    i += 1
                } else if !a.hasPrefix("-psn") {
                    return .failure(.message("Unknown option \(a)"))
                }
            }
            i += 1
        }
        return .success(o)
    }

    /// Runs a command-line mode and returns its exit status; nil starts the app.
    static func command(_ args: [String]) -> Int32? {
        if args.contains("--version") {
            print("Bromelia \(Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "")")
            return 0
        }
        if args.contains("--help") && (args.contains("--headless") || args.count == 1) { print(usage); return 0 }
        if args.contains("--uninstall-agent") {
            if let e = LaunchAgent.uninstall() { FileHandle.standardError.write(Data((e + "\n").utf8)); return 1 }
            print("Removed the background agent")
            return 0
        }
        guard args.contains("--headless") || args.contains("--install-agent") else { return nil }
        let options: Options
        switch parse(args) {
        case .success(let o): options = o
        case .failure(let e):
            FileHandle.standardError.write(Data("\(e.localizedDescription)\n\(usage)\n".utf8))
            return 2
        }
        if args.contains("--install-agent") {
            let passOn = args.filter { $0 != "--install-agent" && $0 != "--headless" }
            if let e = LaunchAgent.install(arguments: passOn) { FileHandle.standardError.write(Data((e + "\n").utf8)); return 1 }
            print("Bromelia now runs in the background at every login (\(LaunchAgent.plistURL.path)); log: \(LaunchAgent.logURL.path)")
            return 0
        }
        return MainActor.assumeIsolated { run(options) }
    }

    @MainActor private static var model: AppModel?
    @MainActor private static var stopping = false
    @MainActor private static var lastStates: [UUID: JobState] = [:]
    @MainActor private static var lastError: String?
    private static var signals: [DispatchSourceSignal] = []

    static func say(_ text: String) {
        print("\(RipJob.logTimeFormatter.string(from: Date())) \(text)")
        fflush(stdout)
    }

    @MainActor
    private static func run(_ o: Options) -> Int32 {
        if let c = o.config { Paths.configOverride = URL(fileURLWithPath: Paths.expandTilde(c)) }
        let app = NSApplication.shared
        app.setActivationPolicy(.prohibited)
        let m = AppModel(persistent: true, headless: true)
        let token = o.token ?? ProcessInfo.processInfo.environment["BROMELIA_WEB_TOKEN"]
        if o.listen != nil || o.port != nil || token != nil {
            // The file keeps its own web settings.
            m.savedWebUI = m.config.webUI
            var w = m.config.webUI
            w.enabled = true
            if let l = o.listen { w.address = l }
            if let p = o.port { w.port = p }
            if let t = token { w.token = t }
            m.config.webUI = w
        }
        model = m
        say("Bromelia \(Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? ""); configuration \(Paths.configFile.path)")
        say("makemkvcon: \(m.makemkvcon?.path ?? "not found — install MakeMKV or set makemkvconPath")")
        say("Output folder: \(m.config.outputRoot)")
        if let e = m.lastError { say(e) }
        m.start()
        if !m.ownsAutomation {
            say("Another Bromelia (\(m.automationLock.holder)) rips inserted discs; this one takes over when it quits")
        }
        if m.config.webUI.enabled {
            let w = m.config.webUI
            say(m.web.lastError.map { "Web page: \($0)" } ?? "Web page: \(w.tlsCertificate.isEmpty ? "http" : "https")://\(w.address):\(w.port)/")
        }
        for sig in [SIGINT, SIGTERM] {
            signal(sig, SIG_IGN)
            let s = DispatchSource.makeSignalSource(signal: sig, queue: .main)
            s.setEventHandler { MainActor.assumeIsolated { stop() } }
            s.resume()
            signals.append(s)
        }
        Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { _ in MainActor.assumeIsolated { report() } }
        app.run()
        return 0
    }

    /// One line per job state change, and new messages.
    @MainActor
    private static func report() {
        guard let m = model else { return }
        for j in m.jobs where lastStates[j.id] != j.state {
            lastStates[j.id] = j.state
            if j.state.isFinished {
                say("\(j.title): \(j.state.label)\(j.outputDirectory.map { " — \($0.path)" } ?? "")\(j.errorMessage.map { " — \($0)" } ?? "")")
            } else {
                say("\(j.title): \(j.state.label) (\(j.mode.label))")
            }
        }
        if let e = m.lastError, e != lastError { say(e) }
        lastError = m.lastError
        if stopping && m.activeJobCount == 0 { finish() }
    }

    @MainActor
    private static func stop() {
        guard let m = model else { exit(0) }
        if stopping { finish() }
        stopping = true
        say("Stopping: cancelling running jobs (send the signal again to quit at once)")
        for j in m.jobs where !j.state.isFinished { m.cancel(j) }
        if m.activeJobCount == 0 { finish() }
    }

    @MainActor
    private static func finish() {
        model?.saveNow()
        exit(0)
    }
}
