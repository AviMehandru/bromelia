import Foundation

/// A source makemkvcon can open.
enum DiscSource: Hashable, Sendable, Codable {
    /// An optical drive. `index` is MakeMKV's drive number, `devicePath` the OS device (/dev/rdisk4).
    case drive(index: Int, devicePath: String)
    case iso(path: String)
    case folder(path: String)

    /// Source string for `info` / `mkv`. Drives are addressed by device path when known, which is
    /// stable even when MakeMKV renumbers drives.
    var infoArgument: String {
        switch self {
        case let .drive(index, dev): return dev.isEmpty ? "disc:\(index)" : "dev:\(dev)"
        case .iso(let p): return "iso:\(p)"
        case .folder(let p): return "file:\(p)"
        }
    }

    /// `backup` only accepts `disc:N`.
    var backupArgument: String? {
        if case let .drive(index, _) = self { return "disc:\(index)" }
        return nil
    }

    var displayName: String {
        switch self {
        case let .drive(index, dev): return dev.isEmpty ? "Drive \(index)" : dev
        case .iso(let p): return (p as NSString).lastPathComponent
        case .folder(let p): return (p as NSString).lastPathComponent
        }
    }

    var isDrive: Bool { if case .drive = self { return true } else { return false } }
}

/// Everything needed to launch makemkvcon for a given drive configuration: the isolated home
/// folder (holding a dedicated settings.conf) and the profile file.
struct MakeMKVEnvironment: Sendable {
    var executable: URL
    var homeDirectory: URL
    var profilePath: String?
    var settings: [String: String]

    var processEnvironment: [String: String] {
        var env = ProcessInfo.processInfo.environment
        env["HOME"] = homeDirectory.path
        return env
    }

    /// Creates `<home>/Library/MakeMKV/settings.conf` and, if needed, the generated profile.
    static func prepare(executable: URL, config: AppConfig, drive: DriveConfig, home: URL,
                        settingsOverride: [String: String] = [:], selectionOverride: String? = nil) throws -> MakeMKVEnvironment {
        let fm = FileManager.default
        let settingsDir = home.appendingPathComponent("Library/MakeMKV", isDirectory: true)
        try fm.createDirectory(at: settingsDir, withIntermediateDirectories: true)

        var settings = config.effectiveSettings(for: drive)
        // Share MakeMKV's normal data folder (SDF, keys, KEYDB.cfg) unless configured otherwise.
        if (settings["app_DataDir"] ?? "").isEmpty {
            settings["app_DataDir"] = Paths.makemkvUserFolder.path
        } else {
            settings["app_DataDir"] = Paths.expandTilde(settings["app_DataDir"]!)
        }
        // Without an explicit key, reuse the one MakeMKV itself is registered with.
        if (settings["app_Key"] ?? "").isEmpty, let key = MakeMKVEnvironment.installedRegistrationKey() {
            settings["app_Key"] = key
        }
        for (k, v) in settingsOverride { settings[k] = v }
        // Settings with empty values mean "MakeMKV default"; don't write them.
        settings = settings.filter { !$0.value.isEmpty }

        var profilePath: String?
        switch drive.profile.mode {
        case .makemkvDefault:
            if let sel = selectionOverride {
                settings["app_DefaultSelectionString"] = sel
            }
        case .generated:
            let url = home.appendingPathComponent("profile.mmcp.xml")
            let xml = ProfileBuilder.build(drive.profile.generated, selectionOverride: selectionOverride)
            try xml.write(to: url, atomically: true, encoding: .utf8)
            profilePath = url.path
            // A selection string in settings.conf would override the profile's own rule.
            if selectionOverride == nil && !drive.profile.generated.selectionRule.isEmpty {
                settings["app_DefaultSelectionString"] = drive.profile.generated.selectionRule
            } else if let sel = selectionOverride {
                settings["app_DefaultSelectionString"] = sel
            }
        case .customFile:
            let p = Paths.expandTilde(drive.profile.customPath)
            if !p.isEmpty { profilePath = p }
            if let sel = selectionOverride { settings["app_DefaultSelectionString"] = sel }
        }

        let conf = SettingsConf.serialize(settings, header: "Drive configuration “\(drive.name)”")
        try conf.write(to: settingsDir.appendingPathComponent("settings.conf"), atomically: true, encoding: .utf8)
        return MakeMKVEnvironment(executable: executable, homeDirectory: home, profilePath: profilePath, settings: settings)
    }

    /// Settings from MakeMKV's own settings.conf (the real user's, not a drive home).
    static func installedSettings() -> [String: String] {
        let url = Paths.makemkvUserFolder.appendingPathComponent("settings.conf")
        guard let text = try? String(contentsOf: url, encoding: .utf8) else { return [:] }
        return SettingsConf.parse(text)
    }

    static func installedRegistrationKey() -> String? {
        let k = installedSettings()["app_Key"] ?? ""
        return k.isEmpty ? nil : k
    }

    // MARK: Argument construction

    func commonSwitches(rip: RipConfig?, noScan: Bool = true) -> [String] {
        var a = ["-r", "--progress=-same", "--messages=-stdout"]
        if noScan { a.append("--noscan") }
        if let p = profilePath { a.append("--profile=\(p)") }
        if let rip {
            if let m = rip.minLengthSeconds { a.append("--minlength=\(m)") }
            if let c = rip.cacheMB, c > 0 { a.append("--cache=\(c)") }
            if let d = rip.directIO { a.append("--directio=\(d ? "true" : "false")") }
            a += ArgumentSplitter.split(rip.extraArguments)
        }
        return a
    }

    func infoArguments(source: DiscSource, rip: RipConfig?) -> [String] {
        commonSwitches(rip: rip) + ["info", source.infoArgument]
    }

    func mkvArguments(source: DiscSource, title: String, destination: String, rip: RipConfig?) -> [String] {
        commonSwitches(rip: rip) + ["mkv", source.infoArgument, title, destination]
    }

    func backupArguments(source: DiscSource, decrypt: Bool, destination: String, rip: RipConfig?) -> [String]? {
        guard let s = source.backupArgument else { return nil }
        return commonSwitches(rip: rip) + (decrypt ? ["backup", "--decrypt", s, destination] : ["backup", s, destination])
    }

    static func scanArguments() -> [String] {
        ["-r", "--cache=1", "--progress=-same", "--messages=-stdout", "info", "disc:9999"]
    }
}

/// MakeMKV's free beta key, which the developer posts on the forum and replaces about once a month.
enum BetaKey {
    static let pageURL = URL(string: "https://forum.makemkv.com/forum/viewtopic.php?f=5&t=1053")!

    /// The key in the forum post (the `T-…` string in its code block).
    static func parse(html: String) -> String? {
        let keyPattern = /T-[A-Za-z0-9@_]{20,}/
        if let code = html.firstMatch(of: /<code>\s*(T-[A-Za-z0-9@_]{20,})\s*<\/code>/) { return String(code.1) }
        return html.firstMatch(of: keyPattern).map { String($0.0) }
    }

    static func fetch() async throws -> String {
        var request = URLRequest(url: pageURL)
        request.timeoutInterval = 30
        let (data, response) = try await URLSession.shared.data(for: request)
        if let http = response as? HTTPURLResponse, http.statusCode != 200 {
            throw JobError.message("The forum page answered with HTTP \(http.statusCode)")
        }
        guard let key = parse(html: String(decoding: data, as: UTF8.self)) else {
            throw JobError.message("No beta key found on the forum page")
        }
        return key
    }
}

/// Where a file or folder the user opened leads: MakeMKV opens discs (an image, or a folder holding BDMV /
/// VIDEO_TS / HVDVD_TS), not single files, so a file inside a disc structure (an .IFO, .VOB, .mpls, .m2ts,
/// …) opens the disc it belongs to.
enum SourceResolver {
    static let imageExtensions: Set<String> = ["iso", "img", "udf"]
    static let structureFolders: Set<String> = ["BDMV", "VIDEO_TS", "HVDVD_TS"]

    static func source(for url: URL, isDirectory: Bool) -> DiscSource {
        if !isDirectory && imageExtensions.contains(url.pathExtension.lowercased()) { return .iso(path: url.path) }
        // The folder holding BDMV / VIDEO_TS / HVDVD_TS, looking from the item itself up to three levels.
        var u = isDirectory ? url : url.deletingLastPathComponent()
        for _ in 0..<4 {
            if structureFolders.contains(u.lastPathComponent.uppercased()) { return .folder(path: u.deletingLastPathComponent().path) }
            if u.pathComponents.count <= 1 { break }
            u = u.deletingLastPathComponent()
        }
        return .folder(path: url.path)
    }
}
