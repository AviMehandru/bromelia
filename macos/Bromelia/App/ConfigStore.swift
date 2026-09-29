import Foundation

enum ConfigStore {
    static func encoder() -> JSONEncoder {
        let e = JSONEncoder()
        e.outputFormatting = [.prettyPrinted, .sortedKeys]
        e.dateEncodingStrategy = .iso8601
        return e
    }

    static func decoder() -> JSONDecoder {
        let d = JSONDecoder()
        d.dateDecodingStrategy = .iso8601
        return d
    }

    static func load(from url: URL = Paths.configFile) -> AppConfig {
        guard let data = try? Data(contentsOf: url) else { return AppConfig() }
        do {
            return try decoder().decode(AppConfig.self, from: data)
        } catch {
            // Keep the unreadable file for the user instead of silently overwriting it.
            let backup = url.deletingPathExtension().appendingPathExtension("broken-\(Int(Date().timeIntervalSince1970)).json")
            try? FileManager.default.copyItem(at: url, to: backup)
            return AppConfig()
        }
    }

    static func save(_ config: AppConfig, to url: URL = Paths.configFile) {
        do {
            try Paths.ensureDirectory(url.deletingLastPathComponent())
            try encoder().encode(config).write(to: url, options: .atomic)
        } catch {
            NSLog("Bromelia: could not save configuration: \(error)")
        }
    }

    static func loadHistory() -> [HistoryRecord] {
        guard let data = try? Data(contentsOf: Paths.historyFile) else { return [] }
        return (try? decoder().decode([HistoryRecord].self, from: data)) ?? []
    }

    static func saveHistory(_ h: [HistoryRecord]) {
        try? Paths.ensureDirectory(Paths.appSupport)
        try? encoder().encode(h).write(to: Paths.historyFile, options: .atomic)
    }

    /// Exported drive configurations (for sharing between machines / platforms).
    struct ExportBundle: Codable {
        var format = "bromelia-drives"
        var version = 1
        var drives: [DriveConfig]
        var presets: [DrivePreset]
    }
}

/// The shared settings catalog (shared/catalog/settings-catalog.json).
struct SettingsCatalog: Decodable {
    struct Choice: Decodable, Hashable { var value: String; var label: String }
    struct Setting: Decodable, Hashable, Identifiable {
        var key: String
        var label: String
        var type: String
        var `default`: String?
        var help: String?
        var min: Int?
        var max: Int?
        var choices: [Choice]?
        var advanced: Bool?
        var platforms: [String]?
        var id: String { key }
        var isAdvanced: Bool { advanced ?? false }
        var appliesToThisPlatform: Bool { platforms.map { $0.contains("macos") } ?? true }
    }
    struct Section: Decodable, Hashable, Identifiable {
        var id: String
        var title: String
        var settings: [Setting]
    }
    struct Token: Decodable, Hashable { var token: String; var help: String }
    struct SelectionPreset: Decodable, Hashable { var name: String; var rule: String }

    var version: Int
    var sections: [Section]
    var selectionTokens: [Token]
    var selectionPresets: [SelectionPreset]

    var allSettings: [Setting] { sections.flatMap(\.settings) }

    static func load() -> SettingsCatalog {
        if let url = Bundle.main.url(forResource: "settings-catalog", withExtension: "json"),
           let data = try? Data(contentsOf: url),
           let c = try? JSONDecoder().decode(SettingsCatalog.self, from: data) {
            return c
        }
        return SettingsCatalog(version: 0, sections: [], selectionTokens: [], selectionPresets: [])
    }
}

/// Queued, waiting and running jobs live only in memory. So that a crash, a power cut or a forced quit never loses one
/// silently, they are also kept in `unfinished-<pid>.json`. At start, jobs left there by a process that has gone are
/// recorded in the history as interrupted (running: failed) or not started (queued, waiting: cancelled), and a
/// running job's staging folder is made visible as `INCOMPLETE - <id>` with a note.
@MainActor
enum UnfinishedJobs {
    struct Entry: Codable, Equatable {
        var id: UUID
        var title: String
        var driveName: String
        var discName: String
        var mode: RipMode
        var state: JobState
        var outputDirectory: String?
        var logPath: String
        var startedAt: Date?
    }

    struct Contents: Codable {
        var pid: Int32
        /// Identifies the process, so a file left by an earlier process with the same pid is still recognised as stale.
        var instance: String
        var jobs: [Entry]
    }

    static let instance = UUID().uuidString

    static func file(pid: Int32 = getpid()) -> URL { Paths.appSupport.appendingPathComponent("unfinished-\(pid).json") }

    static func save(_ jobs: [Entry]) {
        let url = file()
        guard !jobs.isEmpty else {
            try? FileManager.default.removeItem(at: url)
            return
        }
        try? Paths.ensureDirectory(Paths.appSupport)
        try? ConfigStore.encoder().encode(Contents(pid: getpid(), instance: instance, jobs: jobs)).write(to: url, options: .atomic)
    }

    /// History records for the jobs left by processes that have gone; their files are removed.
    static func recover() -> [HistoryRecord] {
        let fm = FileManager.default
        let dir = Paths.appSupport
        var records: [HistoryRecord] = []
        for name in ((try? fm.contentsOfDirectory(atPath: dir.path)) ?? []).sorted() where name.hasPrefix("unfinished-") && name.hasSuffix(".json") {
            let url = dir.appendingPathComponent(name)
            guard let data = try? Data(contentsOf: url), let c = try? ConfigStore.decoder().decode(Contents.self, from: data) else {
                try? fm.removeItem(at: url)
                continue
            }
            // Still in use by another Bromelia.
            if c.instance == instance || (c.pid != getpid() && isAlive(c.pid)) { continue }
            records += c.jobs.map(record)
            try? fm.removeItem(at: url)
        }
        return records
    }

    static func isAlive(_ pid: Int32) -> Bool { pid > 0 && (kill(pid, 0) == 0 || errno == EPERM) }

    private static func record(_ e: Entry) -> HistoryRecord {
        let running = e.state == .running
        let kept = running ? e.outputDirectory.flatMap { recoverStaging(URL(fileURLWithPath: $0, isDirectory: true), id: e.id, logPath: e.logPath) } : nil
        let error = !running ? "Not started: Bromelia stopped while this job was waiting"
            : kept.map { "Interrupted: Bromelia stopped while this job was running; its files were kept in \($0.path)" }
            ?? "Interrupted: Bromelia stopped while this job was running; nothing was saved"
        return HistoryRecord(id: e.id, title: e.title, driveName: e.driveName, discName: e.discName, mode: e.mode,
                             state: running ? .failed : .cancelled, startedAt: e.startedAt, finishedAt: Date(),
                             outputDirectory: kept?.path, files: [], errorMessage: error, logPath: e.logPath,
                             warnings: 0, errors: running ? 1 : 0)
    }

    /// The staging folder of a job that was running: made visible with a note, or removed when nothing was saved.
    /// Returns where its files are now.
    static func recoverStaging(_ outputDir: URL, id: UUID, logPath: String) -> URL? {
        let fm = FileManager.default
        let shortId = String(id.uuidString.prefix(8)).lowercased()
        let stage = outputDir.appendingPathComponent(JobRunner.stagingPrefix + shortId, isDirectory: true)
        guard JobRunner.isDirectory(stage) else { return nil }
        if JobRunner.visibleItems(stage).isEmpty {
            try? fm.removeItem(at: stage) // only temporary (hidden) files
            // The folder made for the job, if nothing else is in it.
            if ((try? fm.contentsOfDirectory(atPath: outputDir.path)) ?? ["?"]).allSatisfy({ $0 == ".DS_Store" }) {
                try? fm.removeItem(at: outputDir)
            }
            return nil
        }
        let dest = Paths.uniqueURL(outputDir.appendingPathComponent("INCOMPLETE - \(shortId)", isDirectory: true))
        guard (try? fm.moveItem(at: stage, to: dest)) != nil else { return stage }
        let note = """
            Bromelia job \(id.uuidString): interrupted.
            Bromelia stopped (it quit, crashed or lost power) while this job was running.
            These files are NOT a finished archive. Rip the disc again.

            Log up to the interruption: \(logPath)

            """
        try? note.write(to: dest.appendingPathComponent("INCOMPLETE.txt"), atomically: true, encoding: .utf8)
        return dest
    }
}
