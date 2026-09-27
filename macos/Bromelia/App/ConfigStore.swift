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
