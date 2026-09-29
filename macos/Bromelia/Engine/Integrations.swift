import Foundation
import DiskArbitration

// MARK: - Notifications

/// Sends job notifications to webhooks, ntfy, Discord, Slack, or any Apprise URL (with the `apprise` command).
enum NotificationSender {
    enum Delivery: Equatable {
        /// An HTTP request: method POST, the URL, headers and body.
        case http(url: URL, headers: [String: String], body: Data)
        /// `apprise -t title -b body url`.
        case apprise(url: String)
    }

    /// How `target` is reached. Returns nil for an empty or malformed URL.
    static func delivery(for target: String, title: String, body: String, status: String) -> Delivery? {
        let raw = target.trimmingCharacters(in: .whitespaces)
        guard let comps = URLComponents(string: raw), let scheme = comps.scheme?.lowercased() else { return nil }
        func json(_ o: [String: Any]) -> Data { (try? JSONSerialization.data(withJSONObject: o, options: [.sortedKeys])) ?? Data() }
        switch scheme {
        case "http", "https":
            guard let url = comps.url, let host = comps.host?.lowercased() else { return nil }
            let path = comps.path
            if (host.hasSuffix("discord.com") || host.hasSuffix("discordapp.com")) && path.contains("/api/webhooks/") {
                return .http(url: url, headers: ["Content-Type": "application/json"], body: json(["content": "**\(title)**\n\(body)"]))
            }
            if host == "hooks.slack.com" {
                return .http(url: url, headers: ["Content-Type": "application/json"], body: json(["text": "*\(title)*\n\(body)"]))
            }
            if host == "ntfy.sh" {
                return .http(url: url, headers: ["Title": title, "Tags": status == "success" ? "white_check_mark" : "warning"], body: Data(body.utf8))
            }
            return .http(url: url, headers: ["Content-Type": "application/json"],
                         body: json(["title": title, "body": body, "status": status, "app": "Bromelia"]))
        case "ntfy", "ntfys":
            // Apprise's form: ntfy://topic (ntfy.sh), ntfy://host/topic, ntfys://host/topic.
            let host = comps.host ?? ""
            let parts = comps.path.split(separator: "/").map(String.init)
            let https = scheme == "ntfys"
            let urlString: String
            if parts.isEmpty { urlString = "https://ntfy.sh/\(host)" } else { urlString = "\(https ? "https" : "http")://\(host)/\(parts.joined(separator: "/"))" }
            guard let url = URL(string: urlString) else { return nil }
            return .http(url: url, headers: ["Title": title, "Tags": status == "success" ? "white_check_mark" : "warning"], body: Data(body.utf8))
        default:
            return .apprise(url: raw)
        }
    }

    /// Sends to every enabled target that wants this status. Failures are reported through `log`.
    static func send(_ targets: [NotificationTarget], title: String, body: String, status: String,
                     log: @escaping @Sendable (String) -> Void) async {
        for t in targets where t.enabled && !(t.onlyProblems && status == "success") {
            guard let d = delivery(for: t.url, title: title, body: body, status: status) else {
                log("Notification: can't use “\(t.url)”")
                continue
            }
            switch d {
            case let .http(url, headers, data):
                var r = URLRequest(url: url)
                r.httpMethod = "POST"
                r.timeoutInterval = 20
                for (k, v) in headers { r.setValue(v, forHTTPHeaderField: k) }
                r.httpBody = data
                do {
                    let (_, resp) = try await URLSession.shared.data(for: r)
                    if let h = resp as? HTTPURLResponse, !(200..<300).contains(h.statusCode) {
                        log("Notification to \(url.host ?? "") failed: HTTP \(h.statusCode)")
                    }
                } catch {
                    log("Notification to \(url.host ?? "") failed: \(error.localizedDescription)")
                }
            case .apprise(let url):
                guard let apprise = EpisodeSplitter.findTool("apprise") else {
                    log("Notification: “\(url)” needs the apprise command (pip install apprise)")
                    continue
                }
                let out = try? await ProcessRunner(executable: apprise, arguments: ["-t", title, "-b", body, url]).run(timeout: 60) { _ in }
                if out?.exitCode != 0 { log("Notification with apprise failed (exit status \(out?.exitCode ?? -1))") }
            }
        }
    }
}

// MARK: - Metadata

/// A movie or show found online.
struct MediaMatch: Codable, Equatable, Sendable {
    var title: String
    var year: Int?
    var tmdbId: Int?
    var imdbId: String?
    var provider: String
}

/// Looks up the canonical title and year of a movie or show on TMDb or OMDb.
enum MetadataLookup {
    static func normalize(_ s: String) -> String {
        s.lowercased().unicodeScalars.filter { CharacterSet.alphanumerics.contains($0) }.map(String.init).joined()
    }

    /// The request for `name`: TMDb takes an API key (v3) or a read access token (Bearer).
    static func request(name: String, kind: MediaKind, config: MetadataConfig) -> URLRequest? {
        let key = config.apiKey.trimmingCharacters(in: .whitespaces)
        guard !key.isEmpty, !name.isEmpty else { return nil }
        var c: URLComponents
        switch config.provider {
        case .none:
            return nil
        case .tmdb:
            c = URLComponents(string: "https://api.themoviedb.org/3/search/\(kind == .tv ? "tv" : "movie")")!
            c.queryItems = [URLQueryItem(name: "query", value: name), URLQueryItem(name: "language", value: config.language)]
            if key.count <= 40 { c.queryItems?.append(URLQueryItem(name: "api_key", value: key)) }
        case .omdb:
            c = URLComponents(string: "https://www.omdbapi.com/")!
            c.queryItems = [URLQueryItem(name: "apikey", value: key), URLQueryItem(name: "t", value: name),
                            URLQueryItem(name: "type", value: kind == .tv ? "series" : "movie")]
        }
        guard let url = c.url else { return nil }
        var r = URLRequest(url: url)
        r.timeoutInterval = 20
        if config.provider == .tmdb && key.count > 40 { r.setValue("Bearer \(key)", forHTTPHeaderField: "Authorization") }
        return r
    }

    /// Picks the result whose title matches `name` (ignoring case and punctuation), else the first one.
    static func parse(_ data: Data, provider: MetadataProvider, name: String) -> MediaMatch? {
        guard let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
        func year(_ s: Any?) -> Int? { (s as? String).flatMap { Int($0.prefix(4)) } }
        switch provider {
        case .none:
            return nil
        case .tmdb:
            let results = obj["results"] as? [[String: Any]] ?? []
            let want = normalize(name)
            let pick = results.first { normalize(($0["title"] ?? $0["name"]) as? String ?? "") == want } ?? results.first
            guard let r = pick, let title = (r["title"] ?? r["name"]) as? String, !title.isEmpty else { return nil }
            return MediaMatch(title: title, year: year(r["release_date"] ?? r["first_air_date"]), tmdbId: r["id"] as? Int, imdbId: nil, provider: "TMDb")
        case .omdb:
            guard (obj["Response"] as? String) == "True", let title = obj["Title"] as? String else { return nil }
            return MediaMatch(title: title, year: year(obj["Year"]), tmdbId: nil, imdbId: obj["imdbID"] as? String, provider: "OMDb")
        }
    }

    static func lookup(name: String, kind: MediaKind, config: MetadataConfig) async throws -> MediaMatch? {
        guard let r = request(name: name, kind: kind, config: config) else { return nil }
        let (data, resp) = try await URLSession.shared.data(for: r)
        if let h = resp as? HTTPURLResponse, h.statusCode != 200 { throw JobError.message("\(config.provider.label) answered HTTP \(h.statusCode)") }
        return parse(data, provider: config.provider, name: name)
    }
}

// MARK: - Media server naming

/// Names for Plex / Jellyfin / Emby libraries.
enum MediaServerNaming {
    static let folderTemplate = "{libraryFolder}/{name}{releaseYear? ({releaseYear})}"
    static let mainTemplate = "{name}{releaseYear? ({releaseYear})}"
    static let episodeTemplate = "Season {seasonOr1:2}/{name}{releaseYear? ({releaseYear})} - S{seasonOr1:2}E{episodeNumber:2}"
    static let otherTemplate = "Other/{name}{releaseYear? ({releaseYear})} - {track}"
    static let backupTemplate = "Backup/{name}{releaseYear? ({releaseYear})} - Backup - {format}"

    /// The file name template for one file: an episode, the main feature of a movie, or another title.
    static func fileTemplate(values: [String: String], isMainFeature: Bool) -> String {
        if !(values["episodeNumber"] ?? "").isEmpty { return episodeTemplate }
        if isMainFeature && values["kind"] == MediaKind.movie.rawValue { return mainTemplate }
        return otherTemplate
    }
}

// MARK: - Other discs

enum DiscContent: Equatable, Sendable {
    case video, audio, data, unknown

    /// The mode for a disc in an automatic or quick rip: the drive's (per-format) mode for DVDs and Blu-rays,
    /// `audioCD` / `dataImage` for other discs when the drive is set up for them, nil to leave the disc alone.
    static func mode(flags: DiscFlags, content: () -> DiscContent, drive: DriveConfig) -> RipMode? {
        if flags.contains(.dvdFiles) || flags.contains(.blurayFiles) || flags.contains(.hdDvdFiles) { return drive.rip.mode }
        switch content() {
        case .video, .unknown: return drive.rip.mode
        case .audio: return drive.other.ripAudioCDs ? .audioCD : nil
        case .data: return drive.other.imageDataDiscs ? .dataImage : nil
        }
    }
}

/// What kind of disc is in a drive, when MakeMKV reports no DVD / Blu-ray structure.
enum DiscContentProbe {
    /// macOS mounts audio CDs with the cddafs file system.
    static func probe(device: String) -> DiscContent {
        let bsd = device.replacingOccurrences(of: "/dev/r", with: "").replacingOccurrences(of: "/dev/", with: "")
        guard !bsd.isEmpty, let session = DASessionCreate(kCFAllocatorDefault),
              let disk = DADiskCreateFromBSDName(kCFAllocatorDefault, session, bsd),
              let desc = DADiskCopyDescription(disk) as? [CFString: Any] else { return .unknown }
        if let kind = desc[kDADiskDescriptionVolumeKindKey] as? String, kind == "cddafs" { return .audio }
        // A DVD / Blu-ray structure on the mounted volume means a video disc, whatever MakeMKV's flags said.
        if let path = (desc[kDADiskDescriptionVolumePathKey] as? URL)?.path,
           let names = try? FileManager.default.contentsOfDirectory(atPath: path),
           names.contains(where: { ["BDMV", "VIDEO_TS", "HVDVD_TS"].contains($0.uppercased()) }) {
            return .video
        }
        if let media = desc[kDADiskDescriptionMediaKindKey] as? String, media == "IOCDMedia" || media == "IODVDMedia" || media == "IOBDMedia" {
            return .data
        }
        return .unknown
    }
}

/// Commands for discs MakeMKV doesn't handle.
enum OtherDiscTools {
    /// The audio CD command: the configured one, else cyanrip, else abcde. Runs in the output folder.
    static func audioCommand(_ config: OtherDiscsConfig, device: String) -> (URL, [String])? {
        let custom = config.audioCommand.trimmingCharacters(in: .whitespaces)
        if !custom.isEmpty {
            let parts = ArgumentSplitter.split(custom).map { TemplateRenderer.render($0, values: ["device": device]) }
            guard let first = parts.first else { return nil }
            let exe = first.hasPrefix("/") ? URL(fileURLWithPath: first) : EpisodeSplitter.findTool(first)
            return exe.map { ($0, Array(parts.dropFirst())) }
        }
        if let cyanrip = EpisodeSplitter.findTool("cyanrip") { return (cyanrip, ["-d", device, "-o", "flac"]) }
        if let abcde = EpisodeSplitter.findTool("abcde") { return (abcde, ["-d", device, "-o", "flac", "-N"]) }
        return nil
    }

    /// Copies a data disc into `destination` (an .iso path) with hdiutil (raw 2048-byte sectors).
    static func imageCommand(device: String, destination: URL) -> (URL, [String]) {
        let bsd = device.replacingOccurrences(of: "/dev/r", with: "/dev/")
        let base = destination.deletingPathExtension().path
        return (URL(fileURLWithPath: "/usr/bin/hdiutil"), ["create", "-srcdevice", bsd, "-format", "UDTO", "-o", base])
    }
}

// MARK: - Trays and mounting

enum DriveControl {
    /// Closes the tray of the drive whose MakeMKV name contains a drive from `drutil list` (by vendor and product).
    /// macOS has no per-device tray call; drives with the same model are closed together.
    static func closeTray(driveName: String) async -> Bool {
        let list = LineCollector()
        guard let out = try? await ProcessRunner(executable: URL(fileURLWithPath: "/usr/bin/drutil"), arguments: ["list"]).run(timeout: 20, onLine: { list.append($0) }),
              out.exitCode == 0 else { return false }
        let indices = drutilIndices(list.all, matching: driveName)
        var ok = !indices.isEmpty
        for i in indices {
            let r = try? await ProcessRunner(executable: URL(fileURLWithPath: "/usr/bin/drutil"), arguments: ["-drive", String(i), "tray", "close"]).run(timeout: 30) { _ in }
            ok = ok && r?.exitCode == 0
        }
        return ok
    }

    /// Drive numbers from `drutil list` whose product name appears in `driveName` (all drives when it is empty).
    static func drutilIndices(_ lines: [String], matching driveName: String) -> [Int] {
        let name = DriveMatch.normalize(driveName)
        var out: [Int] = []
        for line in lines {
            let f = line.split(separator: " ", omittingEmptySubsequences: true).map(String.init)
            guard f.count >= 3, let n = Int(f[0]) else { continue }
            if name.isEmpty || name.contains(f[2].lowercased()) { out.append(n) }
        }
        return out
    }

    /// Waits until the system has mounted the disc in `device`, up to `seconds`. Returns whether it did.
    static func waitForMount(device: String, seconds: Int, isCancelled: @escaping @Sendable () -> Bool) async -> Bool {
        let deadline = Date().addingTimeInterval(TimeInterval(seconds))
        while Date() < deadline && !isCancelled() {
            if await EpisodeSplitter.mountPoint(device: device) != nil { return true }
            try? await Task.sleep(nanoseconds: 2_000_000_000)
        }
        return false
    }
}
