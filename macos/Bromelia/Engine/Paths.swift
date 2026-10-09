import Foundation

enum Paths {
    /// Overrides the data folder (tests, or BROMELIA_DATA_DIR in the environment).
    nonisolated(unsafe) static var dataOverride: URL? = ProcessInfo.processInfo.environment["BROMELIA_DATA_DIR"].map { URL(fileURLWithPath: $0) }

    static var appSupport: URL {
        if let o = dataOverride { return o }
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
        return base.appendingPathComponent("Bromelia", isDirectory: true)
    }

    /// Another configuration file (`--config` of the headless mode).
    nonisolated(unsafe) static var configOverride: URL?
    static var configFile: URL { configOverride ?? appSupport.appendingPathComponent("config.json") }
    static var historyFile: URL { appSupport.appendingPathComponent("history.json") }
    static var jobsDirectory: URL { appSupport.appendingPathComponent("jobs", isDirectory: true) }
    static var drivesDirectory: URL { appSupport.appendingPathComponent("drives", isDirectory: true) }

    /// MakeMKV's own settings folder for the real user (used for importing settings and as shared data dir).
    static var makemkvUserFolder: URL {
        FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/MakeMKV", isDirectory: true)
    }

    static func expandTilde(_ path: String) -> String {
        (path as NSString).expandingTildeInPath
    }

    static func ensureDirectory(_ url: URL) throws {
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
    }

    static let makemkvconCandidates = [
        "/Applications/MakeMKV.app/Contents/MacOS/makemkvcon",
        "~/Applications/MakeMKV.app/Contents/MacOS/makemkvcon",
        "/opt/homebrew/bin/makemkvcon",
        "/usr/local/bin/makemkvcon",
    ]

    static let mkvmergeCandidates = [
        "/opt/homebrew/bin/mkvmerge",
        "/usr/local/bin/mkvmerge",
        "/Applications/MKVToolNix.app/Contents/MacOS/mkvmerge",
        "~/Applications/MKVToolNix.app/Contents/MacOS/mkvmerge",
    ]

    static func resolveTool(configured: String, candidates: [String]) -> URL? {
        let fm = FileManager.default
        let c = expandTilde(configured.trimmingCharacters(in: .whitespaces))
        if !c.isEmpty { return fm.isExecutableFile(atPath: c) ? URL(fileURLWithPath: c) : nil }
        for cand in candidates {
            let p = expandTilde(cand)
            if fm.isExecutableFile(atPath: p) { return URL(fileURLWithPath: p) }
        }
        // Also search in the MKVToolNix app bundle with a version suffix (MKVToolNix-89.0.app).
        if let apps = try? fm.contentsOfDirectory(atPath: "/Applications") {
            for app in apps where app.hasPrefix("MKVToolNix") && candidates == mkvmergeCandidates {
                let p = "/Applications/\(app)/Contents/MacOS/mkvmerge"
                if fm.isExecutableFile(atPath: p) { return URL(fileURLWithPath: p) }
            }
        }
        return nil
    }

    /// Returns `url` or, when it already exists, `url (2)`, `url (3)`, ...
    static func uniqueURL(_ url: URL) -> URL {
        let fm = FileManager.default
        guard fm.fileExists(atPath: url.path) else { return url }
        let ext = url.pathExtension
        let base = ext.isEmpty ? url.lastPathComponent : String(url.lastPathComponent.dropLast(ext.count + 1))
        let dir = url.deletingLastPathComponent()
        var n = 2
        while true {
            let name = ext.isEmpty ? "\(base) (\(n))" : "\(base) (\(n)).\(ext)"
            let cand = dir.appendingPathComponent(name, isDirectory: ext.isEmpty)
            if !fm.fileExists(atPath: cand.path) { return cand }
            n += 1
        }
    }

    /// A free name for something another program will create (MakeMKV makes a backup's destination itself, so nothing
    /// holds the name until it has): `<stem><ext>`, else `<stem> (2)<ext>` and so on, held meanwhile by a hidden marker
    /// next to it (`.<name>.bromelia`, created exclusively), so that two backups started together can't pick the same
    /// one. Remove the marker once the thing has its name. No marker when the folder can't take one.
    static func reserveUnique(in dir: URL, stem: String, ext: String) -> (url: URL, marker: URL?) {
        let fm = FileManager.default
        var n = 1
        while true {
            let leaf = n == 1 ? stem + ext : "\(stem) (\(n))\(ext)"
            n += 1
            let url = dir.appendingPathComponent(leaf, isDirectory: ext.isEmpty)
            let marker = dir.appendingPathComponent(".\(leaf).bromelia")
            if fm.fileExists(atPath: url.path) || fm.fileExists(atPath: marker.path) { continue }
            let fd = open(marker.path, O_CREAT | O_EXCL | O_WRONLY, 0o644)
            if fd < 0 {
                if errno == EEXIST { continue }
                return (url, nil)
            }
            close(fd)
            if fm.fileExists(atPath: url.path) {
                unlink(marker.path)
                continue
            }
            return (url, marker)
        }
    }
}
