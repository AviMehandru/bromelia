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
        guard let sep = raw.range(of: "://"), sep.lowerBound > raw.startIndex else { return nil }
        let scheme = raw[..<sep.lowerBound].lowercased()
        if ["tgram", "pover", "gotify", "gotifys"].contains(scheme) {
            return appriseStyle(scheme: scheme, rest: String(raw[sep.upperBound...]), title: title, body: body, status: status) ?? .apprise(url: raw)
        }
        guard let comps = URLComponents(string: raw) else { return scheme == "http" || scheme == "https" ? nil : .apprise(url: raw) }
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

    /// Apprise URLs sent directly, without Python: Telegram with one chat (`tgram://bot_token/chat_id`), Pushover
    /// (`pover://user_key@app_token[/device]`) and Gotify (`gotify://host[:port][/path]/app_token`, `gotifys://` for
    /// HTTPS). nil for the other forms, which go to the apprise command.
    static func appriseStyle(scheme: String, rest: String, title: String, body: String, status: String) -> Delivery? {
        func json(_ o: [String: Any]) -> Data { (try? JSONSerialization.data(withJSONObject: o, options: [.sortedKeys])) ?? Data() }
        let parts = rest.split(separator: "/", omittingEmptySubsequences: false).map(String.init)
        let path = parts.last == "" ? Array(parts.dropLast()) : parts
        switch scheme {
        case "tgram":
            guard path.count == 2, path[0].contains(":"), !path[1].isEmpty,
                  let url = URL(string: "https://api.telegram.org/bot\(path[0])/sendMessage") else { return nil }
            return .http(url: url, headers: ["Content-Type": "application/json"], body: json(["chat_id": path[1], "text": "\(title)\n\(body)"]))
        case "pover":
            guard (1...2).contains(path.count), let at = path[0].firstIndex(of: "@") else { return nil }
            let user = String(path[0][..<at]), token = String(path[0][path[0].index(after: at)...])
            guard !user.isEmpty, !token.isEmpty, let url = URL(string: "https://api.pushover.net/1/messages.json") else { return nil }
            var o: [String: Any] = ["token": token, "user": user, "title": title, "message": body]
            if path.count == 2, !path[1].isEmpty { o["device"] = path[1] }
            return .http(url: url, headers: ["Content-Type": "application/json"], body: json(o))
        default:
            guard path.count >= 2, !path[0].isEmpty, let token = path.last, !token.isEmpty,
                  let url = URL(string: "\(scheme == "gotifys" ? "https" : "http")://" + path.dropLast().joined(separator: "/") + "/message") else { return nil }
            return .http(url: url, headers: ["Content-Type": "application/json", "X-Gotify-Key": token],
                         body: json(["title": title, "message": body, "priority": status == "success" ? 5 : 8]))
        }
    }

    /// The apprise command, else Python with the apprise module (`python3 -m apprise`).
    static func appriseCommand() -> (URL, [String])? {
        if let a = EpisodeSplitter.findTool("apprise") { return (a, []) }
        if let py = EpisodeSplitter.findTool("python3") { return (py, ["-m", "apprise"]) }
        return nil
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
                guard let (apprise, prefix) = appriseCommand() else {
                    log("Notification: “\(url)” needs Apprise (pip install apprise)")
                    continue
                }
                let out = try? await ProcessRunner(executable: apprise, arguments: prefix + ["-t", title, "-b", body, url]).run(timeout: 60) { _ in }
                if out?.exitCode != 0 { log("Notification with apprise failed (exit status \(out?.exitCode ?? -1))") }
            }
        }
    }
}

// MARK: - Metadata

/// A movie or show found online.
struct MediaMatch: Codable, Equatable, Hashable, Sendable {
    var title: String
    var year: Int?
    var tmdbId: Int?
    var imdbId: String?
    var provider: String
    /// Movie or TV show, as the provider lists it.
    var kind: MediaKind?
    /// Plot summary (TMDb overview, OMDb plot), for .nfo files.
    var overview = ""
    /// Poster image URL, or "".
    var poster = ""

    /// "The Matrix (1999)".
    var label: String { year.map { "\(title) (\($0))" } ?? title }

    /// What to type (or pick) on the disc page to choose this match: "movie/603", "tv/1668" or "tt0133093".
    var choiceId: String {
        if let tmdbId { return "\((kind ?? .movie).rawValue)/\(tmdbId)" }
        return imdbId ?? ""
    }
}

/// An episode of a season, as listed online.
struct EpisodeDetails: Equatable, Sendable {
    var title: String
    var overview = ""
    /// yyyy-mm-dd, or "".
    var aired = ""
}

/// A movie or show chosen by its id: a TMDb id (`603`, `tmdb:603`, `movie/603`, `tv/1668`, a themoviedb.org address)
/// or an IMDb id (`tt0133093`, an imdb.com address).
enum OnlineId: Equatable, Sendable {
    case tmdb(Int, MediaKind?)
    case imdb(String)

    /// "TMDb movie/603", "TMDb 603", "tt0133093".
    var label: String {
        switch self {
        case let .tmdb(n, k): return "TMDb " + (k.map { "\($0.rawValue)/\(n)" } ?? String(n))
        case .imdb(let tt): return tt
        }
    }

    static func parse(_ text: String) -> OnlineId? {
        let t = text.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        if let m = t.firstMatch(of: /tt\d{5,10}/) { return .imdb(String(m.0)) }
        if let m = t.firstMatch(of: /themoviedb\.org\/(movie|tv)\/(\d+)/) {
            return Int(m.2).flatMap { $0 > 0 ? .tmdb($0, MediaKind(rawValue: String(m.1))) : nil }
        }
        if let m = t.wholeMatch(of: /(?:tmdb:)?(?:(movie|tv)\/)?(\d{1,9})/), let n = Int(m.2), n > 0 {
            return .tmdb(n, m.1.flatMap { MediaKind(rawValue: String($0)) })
        }
        return nil
    }
}

/// Looks up movies and shows on TMDb or OMDb: candidates for a name (ranked), a movie or show by its id, and the
/// episodes of a season.
enum MetadataLookup {
    static func normalize(_ s: String) -> String {
        s.lowercased().unicodeScalars.filter { CharacterSet.alphanumerics.contains($0) }.map(String.init).joined()
    }

    /// A year typed after the name: "Inception (2010)" → ("Inception", 2010).
    static func splitYear(_ name: String) -> (name: String, year: Int?) {
        let t = name.trimmingCharacters(in: .whitespaces)
        if let m = t.wholeMatch(of: /(.*\S)\s*\((\d{4})\)/), let y = Int(m.2), (1870...2100).contains(y) { return (String(m.1), y) }
        return (t, nil)
    }

    private static func key(_ config: MetadataConfig) -> String { config.apiKey.trimmingCharacters(in: .whitespaces) }

    /// A TMDb request: an API key (v3) goes into the query, a read access token into a Bearer header.
    private static func tmdb(_ path: String, _ query: [URLQueryItem], config: MetadataConfig) -> URLRequest? {
        let key = key(config)
        guard !key.isEmpty, var c = URLComponents(string: "https://api.themoviedb.org/3/" + path) else { return nil }
        c.queryItems = query + [URLQueryItem(name: "language", value: config.language.isEmpty ? "en-US" : config.language)]
        if key.count <= 40 { c.queryItems?.append(URLQueryItem(name: "api_key", value: key)) }
        guard let url = c.url else { return nil }
        var r = URLRequest(url: url)
        r.timeoutInterval = 20
        if key.count > 40 { r.setValue("Bearer \(key)", forHTTPHeaderField: "Authorization") }
        return r
    }

    private static func omdb(_ query: [URLQueryItem], config: MetadataConfig) -> URLRequest? {
        let key = key(config)
        guard !key.isEmpty, var c = URLComponents(string: "https://www.omdbapi.com/") else { return nil }
        c.queryItems = [URLQueryItem(name: "apikey", value: key)] + query
        guard let url = c.url else { return nil }
        var r = URLRequest(url: url)
        r.timeoutInterval = 20
        return r
    }

    /// The search for `name` (optionally released in `year`).
    static func request(name: String, kind: MediaKind, year: Int? = nil, config: MetadataConfig) -> URLRequest? {
        guard !name.isEmpty else { return nil }
        switch config.provider {
        case .none:
            return nil
        case .tmdb:
            var q = [URLQueryItem(name: "query", value: name)]
            if let year { q.append(URLQueryItem(name: kind == .tv ? "first_air_date_year" : "year", value: String(year))) }
            return tmdb("search/\(kind == .tv ? "tv" : "movie")", q, config: config)
        case .omdb:
            var q = [URLQueryItem(name: "s", value: name), URLQueryItem(name: "type", value: kind == .tv ? "series" : "movie")]
            if let year { q.append(URLQueryItem(name: "y", value: String(year))) }
            return omdb(q, config: config)
        }
    }

    /// The movie or show with this id. TMDb ids without a kind use `kind`; OMDb only takes IMDb ids.
    static func request(id: OnlineId, kind: MediaKind, config: MetadataConfig) -> URLRequest? {
        switch (config.provider, id) {
        case let (.tmdb, .tmdb(n, k)): return tmdb("\((k ?? kind) == .tv ? "tv" : "movie")/\(n)", [], config: config)
        case let (.tmdb, .imdb(tt)): return tmdb("find/\(tt)", [URLQueryItem(name: "external_source", value: "imdb_id")], config: config)
        case let (.omdb, .imdb(tt)): return omdb([URLQueryItem(name: "i", value: tt), URLQueryItem(name: "plot", value: "full")], config: config)
        default: return nil
        }
    }

    /// The episodes of `season` of the show `match`.
    static func seasonRequest(match: MediaMatch, season: Int, config: MetadataConfig) -> URLRequest? {
        switch config.provider {
        case .tmdb: return match.tmdbId.flatMap { tmdb("tv/\($0)/season/\(season)", [], config: config) }
        case .omdb: return match.imdbId.flatMap { omdb([URLQueryItem(name: "i", value: $0), URLQueryItem(name: "Season", value: String(season))], config: config) }
        case .none: return nil
        }
    }

    private static func object(_ data: Data) -> [String: Any]? { try? JSONSerialization.jsonObject(with: data) as? [String: Any] }
    private static func year(_ s: Any?) -> Int? { (s as? String).flatMap { Int($0.prefix(4)) } }
    private static func text(_ s: Any?) -> String { (s as? String).map { $0 == "N/A" ? "" : $0 } ?? "" }

    private static func tmdbMatch(_ r: [String: Any], kind: MediaKind) -> MediaMatch? {
        guard let title = (r["title"] ?? r["name"]) as? String, !title.isEmpty else { return nil }
        let poster = (r["poster_path"] as? String).map { "https://image.tmdb.org/t/p/original" + $0 } ?? ""
        let imdb = text(r["imdb_id"])
        return MediaMatch(title: title, year: year(r["release_date"] ?? r["first_air_date"]), tmdbId: r["id"] as? Int,
                          imdbId: imdb.isEmpty ? nil : imdb, provider: "TMDb", kind: kind, overview: text(r["overview"]), poster: poster)
    }

    private static func omdbMatch(_ r: [String: Any], kind: MediaKind?) -> MediaMatch? {
        guard let title = r["Title"] as? String, !title.isEmpty else { return nil }
        let type = r["Type"] as? String
        return MediaMatch(title: title, year: year(r["Year"]), tmdbId: nil, imdbId: r["imdbID"] as? String, provider: "OMDb",
                          kind: type == "series" ? .tv : type == "movie" ? .movie : kind, overview: text(r["Plot"]), poster: text(r["Poster"]))
    }

    /// The search results, best first: the title matching `name` (ignoring case and punctuation) scores 4, the year
    /// hint 2 (1 for a year off by one); ties keep the provider's order.
    static func candidates(_ data: Data, provider: MetadataProvider, name: String, year: Int? = nil, kind: MediaKind) -> [MediaMatch] {
        guard let obj = object(data) else { return [] }
        let list: [MediaMatch]
        switch provider {
        case .none:
            return []
        case .tmdb:
            list = (obj["results"] as? [[String: Any]] ?? []).compactMap { tmdbMatch($0, kind: kind) }
        case .omdb:
            guard (obj["Response"] as? String) == "True" else { return [] }
            if let search = obj["Search"] as? [[String: Any]] {
                list = search.compactMap { omdbMatch($0, kind: kind) }
            } else {
                list = omdbMatch(obj, kind: kind).map { [$0] } ?? []
            }
        }
        let want = normalize(name)
        func score(_ m: MediaMatch) -> Int {
            var s = normalize(m.title) == want ? 4 : 0
            if let year, let y = m.year { s += y == year ? 2 : abs(y - year) == 1 ? 1 : 0 }
            return s
        }
        return list.enumerated().sorted { a, b in
            let sa = score(a.element), sb = score(b.element)
            return sa != sb ? sa > sb : a.offset < b.offset
        }.map(\.element)
    }

    /// The best search result (see `candidates`).
    static func parse(_ data: Data, provider: MetadataProvider, name: String, year: Int? = nil, kind: MediaKind = .movie) -> MediaMatch? {
        candidates(data, provider: provider, name: name, year: year, kind: kind).first
    }

    /// A movie or show read by its id: TMDb details or /find, or OMDb details.
    static func details(_ data: Data, provider: MetadataProvider, kind: MediaKind) -> MediaMatch? {
        guard let obj = object(data) else { return nil }
        switch provider {
        case .none:
            return nil
        case .tmdb:
            if obj["movie_results"] != nil || obj["tv_results"] != nil {
                let movies = obj["movie_results"] as? [[String: Any]] ?? [], shows = obj["tv_results"] as? [[String: Any]] ?? []
                // The kind the id belongs to, whatever the disc was taken for.
                let pick = kind == .tv ? (shows.first.map { ($0, MediaKind.tv) } ?? movies.first.map { ($0, .movie) })
                                       : (movies.first.map { ($0, MediaKind.movie) } ?? shows.first.map { ($0, .tv) })
                return pick.flatMap { tmdbMatch($0.0, kind: $0.1) }
            }
            guard obj["id"] != nil else { return nil }
            return tmdbMatch(obj, kind: obj["name"] != nil && obj["title"] == nil ? .tv : .movie)
        case .omdb:
            guard (obj["Response"] as? String) == "True" else { return nil }
            return omdbMatch(obj, kind: kind)
        }
    }

    /// Episode number → title, plot and air date.
    static func season(_ data: Data, provider: MetadataProvider) -> [Int: EpisodeDetails] {
        guard let obj = object(data) else { return [:] }
        var out: [Int: EpisodeDetails] = [:]
        switch provider {
        case .none:
            break
        case .tmdb:
            for e in obj["episodes"] as? [[String: Any]] ?? [] {
                guard let n = e["episode_number"] as? Int, let t = e["name"] as? String, !t.isEmpty else { continue }
                out[n] = EpisodeDetails(title: t, overview: text(e["overview"]), aired: text(e["air_date"]))
            }
        case .omdb:
            for e in obj["Episodes"] as? [[String: Any]] ?? [] {
                guard let n = (e["Episode"] as? String).flatMap({ Int($0) }), let t = e["Title"] as? String, !t.isEmpty else { continue }
                out[n] = EpisodeDetails(title: t, aired: text(e["Released"]))
            }
        }
        return out
    }

    private static func fetch(_ r: URLRequest, config: MetadataConfig) async throws -> Data {
        let (data, resp) = try await URLSession.shared.data(for: r)
        if let h = resp as? HTTPURLResponse, h.statusCode != 200 { throw JobError.message("\(config.provider.label) answered HTTP \(h.statusCode)") }
        return data
    }

    /// The candidates for `name`, best first. A year that finds nothing is dropped; OMDb's best result is read again for
    /// its plot.
    static func search(name: String, kind: MediaKind, year: Int?, config: MetadataConfig) async throws -> [MediaMatch] {
        var list: [MediaMatch] = []
        if let r = request(name: name, kind: kind, year: year, config: config) {
            list = candidates(try await fetch(r, config: config), provider: config.provider, name: name, year: year, kind: kind)
        }
        if list.isEmpty, year != nil, let r = request(name: name, kind: kind, config: config) {
            list = candidates(try await fetch(r, config: config), provider: config.provider, name: name, year: year, kind: kind)
        }
        if config.provider == .omdb, let tt = list.first?.imdbId, let r = request(id: .imdb(tt), kind: kind, config: config),
           let data = try? await fetch(r, config: config), let full = details(data, provider: .omdb, kind: kind) {
            list[0] = full
        }
        return list
    }

    static func lookup(id: OnlineId, kind: MediaKind, config: MetadataConfig) async throws -> MediaMatch? {
        guard let r = request(id: id, kind: kind, config: config) else {
            throw JobError.message("\(config.provider.label) can't look up \(id.label)\(config.provider == .omdb ? " (it takes IMDb ids, tt…)" : "")")
        }
        return details(try await fetch(r, config: config), provider: config.provider, kind: kind)
    }

    static func episodes(of match: MediaMatch, season: Int, config: MetadataConfig) async throws -> [Int: EpisodeDetails] {
        guard let r = seasonRequest(match: match, season: season, config: config) else { return [:] }
        return self.season(try await fetch(r, config: config), provider: config.provider)
    }
}

// MARK: - Media server naming

/// Names for Plex / Jellyfin / Emby libraries.
enum MediaServerNaming {
    static let folderTemplate = "{libraryFolder}/{name}{releaseYear? ({releaseYear})}"
    static let mainTemplate = "{name}{releaseYear? ({releaseYear})}"
    static let episodeTemplate = "Season {seasonOr1:2}/{name}{releaseYear? ({releaseYear})} - S{seasonOr1:2}E{episodeNumber:2}{episodeTitle? - {episodeTitle}}"
    static let otherTemplate = "Other/{name}{releaseYear? ({releaseYear})} - {track}"
    static let backupTemplate = "Backup/{name}{releaseYear? ({releaseYear})} - Backup - {format}"

    /// The file name template for one file: an episode, the main feature of a movie, or another title.
    static func fileTemplate(values: [String: String], isMainFeature: Bool) -> String {
        if !(values["episodeNumber"] ?? "").isEmpty { return episodeTemplate }
        if isMainFeature && values["kind"] == MediaKind.movie.rawValue { return mainTemplate }
        return otherTemplate
    }
}

// MARK: - Media server metadata

/// Kodi / Jellyfin / Emby metadata written next to a media server library: .nfo files and the poster.
enum MediaServerMetadata {
    static let posterName = "poster.jpg"

    /// Files this writes, which SHA256SUMS doesn't list (media servers may rewrite them).
    static func isMetadataFile(_ name: String) -> Bool { name.lowercased().hasSuffix(".nfo") || name == posterName }

    static func escape(_ s: String) -> String {
        s.replacingOccurrences(of: "&", with: "&amp;").replacingOccurrences(of: "<", with: "&lt;").replacingOccurrences(of: ">", with: "&gt;")
    }

    private static func document(_ root: String, _ elements: [(String, String)]) -> String {
        var out = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<\(root)>\n"
        for (name, value) in elements where !value.isEmpty {
            if name.hasPrefix("uniqueid ") { out += "  <\(name)>\(escape(value))</uniqueid>\n" } else { out += "  <\(name)>\(escape(value))</\(name)>\n" }
        }
        return out + "</\(root)>\n"
    }

    /// movie.nfo / tvshow.nfo: title, year, plot and the ids (the first one is the default).
    static func nfo(_ m: MediaMatch, kind: MediaKind) -> String {
        var e: [(String, String)] = [("title", m.title), ("year", m.year.map(String.init) ?? ""), ("plot", m.overview)]
        var ids: [(String, String)] = []
        if let t = m.tmdbId { ids.append(("tmdb", String(t))) }
        if let i = m.imdbId, !i.isEmpty { ids.append(("imdb", i)) }
        for (n, (type, value)) in ids.enumerated() { e.append(("uniqueid type=\"\(type)\"\(n == 0 ? " default=\"true\"" : "")", value)) }
        return document(kind == .tv ? "tvshow" : "movie", e)
    }

    /// An episode's .nfo.
    static func episodeNfo(show: String, season: Int, episode: Int, details: EpisodeDetails) -> String {
        document("episodedetails", [("title", details.title), ("showtitle", show), ("season", String(season)), ("episode", String(episode)),
                                    ("plot", details.overview), ("aired", details.aired)])
    }

    static func download(_ url: URL, to file: URL) async throws {
        var r = URLRequest(url: url)
        r.timeoutInterval = 60
        let (data, resp) = try await URLSession.shared.data(for: r)
        if let h = resp as? HTTPURLResponse, h.statusCode != 200 { throw JobError.message("HTTP \(h.statusCode)") }
        guard !data.isEmpty else { throw JobError.message("empty answer") }
        try data.write(to: file, options: .withoutOverwriting)
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
