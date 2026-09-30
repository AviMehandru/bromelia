import Foundation

// What a disc *is*: its format (DVD / Blu-ray / 4K UHD), the movie or show on it, and where it sits in a
// set (season, part, volume, disc number). Used for file names, post-processing plugin matching and the
// archive record. The same rules are implemented by the Windows and Linux versions.

enum DiscFormat: String, Codable, Sendable, CaseIterable {
    case dvd, bluray, uhd, hddvd, unknown

    /// Format code used in file names. Encrypted (not decrypted) backups get an `e` suffix.
    func code(encrypted: Bool) -> String {
        let base: String
        switch self {
        case .dvd: base = "DVD"
        case .bluray: base = "BR"
        case .uhd: base = "4K"
        case .hddvd: base = "HDDVD"
        case .unknown: base = "DISC"
        }
        return encrypted ? base + "e" : base
    }

    var label: String {
        switch self {
        case .dvd: return "DVD"
        case .bluray: return "Blu-ray"
        case .uhd: return "4K Ultra HD Blu-ray"
        case .hddvd: return "HD DVD"
        case .unknown: return "Disc"
        }
    }

    /// Every format code, for pickers and validation.
    static let allCodes = ["DVD", "DVDe", "BR", "BRe", "4K", "4Ke", "HDDVD", "HDDVDe"]

    /// Detects the format from makemkvcon's disc listing, falling back to the drive's file system flags.
    /// UHD discs are Blu-rays whose video is 2160p or HEVC.
    static func detect(info: DiscInfo?, flags: DiscFlags? = nil) -> DiscFormat {
        if let info {
            let t = info.typeName.lowercased()
            if t.contains("blu") {
                return isUHD(info) ? .uhd : .bluray
            }
            if t.contains("hd") { return .hddvd }
            if t.contains("dvd") { return .dvd }
            if isUHD(info) { return .uhd }
        }
        if let flags {
            if flags.contains(.blurayFiles) { return .bluray }
            if flags.contains(.hdDvdFiles) { return .hddvd }
            if flags.contains(.dvdFiles) { return .dvd }
        }
        return .unknown
    }

    static func isUHD(_ info: DiscInfo) -> Bool {
        for title in info.titles {
            for track in title.tracks where track.kind == .video {
                let size = track.attribute(.videoSize) ?? ""
                let codec = ((track.attribute(.codecId) ?? "") + " " + (track.attribute(.codecShort) ?? "")).uppercased()
                if size.contains("2160") || size.contains("3840") || codec.contains("HEVC") || codec.contains("MPEGH") { return true }
            }
        }
        return false
    }

    /// Blu-ray backup folders: BDMV/index.bdmv starts with "INDX0300" on UHD discs, "INDX0200"/"INDX0100" otherwise.
    static func detect(backupFolder: URL) -> DiscFormat? {
        let fm = FileManager.default
        if fm.fileExists(atPath: backupFolder.appendingPathComponent("VIDEO_TS").path) { return .dvd }
        guard let h = FileHandle(forReadingAtPath: backupFolder.appendingPathComponent("BDMV/index.bdmv").path) else { return nil }
        defer { try? h.close() }
        let head = String(decoding: h.readData(ofLength: 8), as: UTF8.self)
        guard head.hasPrefix("INDX") else { return nil }
        return head == "INDX0300" ? .uhd : .bluray
    }
}

enum MediaKind: String, Codable, Sendable, CaseIterable, Identifiable {
    case movie, tv
    var id: String { rawValue }
    var label: String { self == .movie ? "Movie" : "TV show" }
}

/// Result of reading a disc label such as `ONE_PIECE_S2_P7_D2` or `The Matrix - Disc 1`.
struct LabelInfo: Equatable, Sendable {
    var title: String = ""
    var season: Int?
    var part: Int?
    var volume: Int?
    var disc: Int?
    /// The label itself says this is a series (season / episode / volume markers).
    var looksLikeSeries = false

    /// "Season 2 Part 7 Disc 2", or "" when the label carries no set information.
    var setDescription: String {
        var parts: [String] = []
        if let season { parts.append("Season \(season)") }
        if let part { parts.append("Part \(part)") }
        if let volume { parts.append("Volume \(volume)") }
        if let disc { parts.append("Disc \(disc)") }
        return parts.joined(separator: " ")
    }
}

enum LabelParser {
    /// Tokens that describe the medium rather than the content; dropped from titles.
    static let noise: Set<String> = ["WS", "FS", "16X9", "4X3", "NTSC", "PAL", "R1", "R2", "R4", "UHD", "4K", "BD", "BLURAY", "BLU", "RAY",
                                     "DVD", "DVD5", "DVD9", "BD25", "BD50", "BD66", "BD100", "HDR", "SDR", "HD", "DISC", "DISK"]
    static let smallWords: Set<String> = ["a", "an", "and", "as", "at", "but", "by", "for", "from", "in", "into", "nor", "of", "on",
                                          "or", "the", "to", "vs", "with"]

    /// Splits a label into words, reads set markers (S2, SEASON 2, P7, VOL 3, D2, DISC 2, S1D2 …) and returns
    /// the cleaned title. Everything from the first set marker on is left out of the title.
    static func parse(_ raw: String) -> LabelInfo {
        var info = LabelInfo()
        var s = raw
        for junk in ["™", "®", "©"] { s = s.replacingOccurrences(of: junk, with: "") }
        // Separators: underscores, dots, whitespace, and dashes surrounded by spaces (hyphenated words stay).
        s = s.replacingOccurrences(of: " - ", with: " ")
        let tokens = s.split(whereSeparator: { $0 == "_" || $0 == "." || $0.isWhitespace || $0 == "(" || $0 == ")" || $0 == "[" || $0 == "]" || $0 == "," })
            .map(String.init).filter { !$0.isEmpty && $0 != "-" }
        var titleTokens: [String] = []
        var stopped = false
        var i = 0
        func number(after idx: Int) -> Int? { idx + 1 < tokens.count ? Int(tokens[idx + 1]) : nil }

        while i < tokens.count {
            let t = tokens[i]
            let u = t.uppercased()
            var consumedNext = false
            var marker = true
            if let m = u.firstMatch(of: /^S(\d{1,2})(?:D(\d{1,2}))?(?:E(\d{1,3}))?$/) {
                info.season = Int(m.1)
                if let d = m.2 { info.disc = Int(d) }
                info.looksLikeSeries = true
            } else if let m = u.firstMatch(of: /^SEASON(\d{1,2})?$/) {
                if let n = m.1.flatMap({ Int($0) }) { info.season = n } else if let n = number(after: i) { info.season = n; consumedNext = true }
                info.looksLikeSeries = true
            } else if let m = u.firstMatch(of: /^(?:EP|EPS|EPISODE|EPISODES)(\d{1,3})?$/) {
                if m.1 == nil, number(after: i) != nil { consumedNext = true }
                info.looksLikeSeries = true
            } else if let m = u.firstMatch(of: /^(?:D|DISC|DISK|CD)(\d{1,2})$/) {
                info.disc = Int(m.1)
            } else if u == "DISC" || u == "DISK" || u == "D", let n = number(after: i) {
                info.disc = n; consumedNext = true
            } else if let m = u.firstMatch(of: /^(?:P|PT|PART)(\d{1,2})$/) {
                info.part = Int(m.1)
            } else if u == "PART" || u == "PT", let n = number(after: i) {
                info.part = n; consumedNext = true
            } else if let m = u.firstMatch(of: /^(?:V|VOL|VOLUME)(\d{1,2})$/) {
                info.volume = Int(m.1); info.looksLikeSeries = true
            } else if u == "VOL" || u == "VOLUME", let n = number(after: i) {
                info.volume = n; consumedNext = true; info.looksLikeSeries = true
            } else {
                marker = false
            }
            if marker {
                stopped = true
            } else if !stopped && !noise.contains(u) {
                titleTokens.append(t)
            }
            i += consumedNext ? 2 : 1
        }
        info.title = titleCase(titleTokens)
        return info
    }

    /// Title-cases words that are all upper or all lower case (labels); mixed-case words are kept.
    static func titleCase(_ words: [String]) -> String {
        let shouting = words.allSatisfy { $0 == $0.uppercased() } || words.allSatisfy { $0 == $0.lowercased() }
        guard shouting else { return words.joined(separator: " ") }
        return words.enumerated().map { i, w in
            let lower = w.lowercased()
            if i > 0 && smallWords.contains(lower) { return lower }
            // Roman numerals stay upper case (Rocky II, Part IV).
            if w.count <= 4, lower.allSatisfy({ "ivx".contains($0) }) { return w.uppercased() }
            return lower.split(separator: "-", omittingEmptySubsequences: false).map { $0.prefix(1).uppercased() + $0.dropFirst() }.joined(separator: "-")
        }.joined(separator: " ")
    }
}

/// The movie or show on a disc, as used for naming. Built by `MediaIdentity.resolve`.
struct MediaIdentity: Equatable, Sendable {
    var name: String
    var kind: MediaKind
    var format: DiscFormat
    var encrypted: Bool
    var label: LabelInfo
    /// How the kind was decided (logged).
    var reason: String

    var formatCode: String { format.code(encrypted: encrypted) }

    /// Titles that look like episodes: 10–75 minutes and within ±35 % of the median of such titles.
    static func episodeLikeTitles(_ titles: [TitleInfo]) -> [TitleInfo] {
        let candidates = titles.filter { (600...4500).contains($0.durationSeconds) }
        guard candidates.count >= 2 else { return [] }
        let sorted = candidates.map(\.durationSeconds).sorted()
        let median = Double(sorted[sorted.count / 2])
        return candidates.filter { abs(Double($0.durationSeconds) - median) <= median * 0.35 }
    }

    /// - Parameters:
    ///   - nameOverride / kindOverride: the user's choice for this disc (empty / nil = infer).
    ///   - playAllEpisodes: episodes found in a DVD "play all" title (see `DVDNavigation`), if known.
    static func resolve(info: DiscInfo?, discLabel: String, flags: DiscFlags? = nil, format: DiscFormat? = nil, encrypted: Bool,
                        nameOverride: String = "", kindOverride: MediaKind? = nil, playAllEpisodes: Int = 0) -> MediaIdentity {
        let volume = info?.volumeName.isEmpty == false ? info!.volumeName : discLabel
        let discName = info?.name ?? ""
        let fromVolume = LabelParser.parse(volume)
        // makemkvcon's disc name is the Blu-ray's metadata title when there is one ("The Matrix"), otherwise
        // the volume label. Prefer it when it looks like a real title (spaces, mixed case, no underscores).
        let nameLooksHuman = !discName.isEmpty && !discName.contains("_") && discName != volume
            && (discName.contains(" ") || discName != discName.uppercased())
        let fromName = LabelParser.parse(discName)
        var label = fromVolume
        if label.title.isEmpty { label.title = fromName.title }
        if label.season == nil { label.season = fromName.season }
        if label.disc == nil { label.disc = fromName.disc }
        if label.part == nil { label.part = fromName.part }
        if label.volume == nil { label.volume = fromName.volume }
        label.looksLikeSeries = fromVolume.looksLikeSeries || fromName.looksLikeSeries

        var name = nameOverride.trimmingCharacters(in: .whitespaces)
        if name.isEmpty { name = nameLooksHuman && !fromName.title.isEmpty ? fromName.title : label.title }
        if name.isEmpty { name = discLabel.isEmpty ? "Disc" : discLabel }

        let fmt = format ?? DiscFormat.detect(info: info, flags: flags)
        let kind: MediaKind
        let reason: String
        if let kindOverride {
            kind = kindOverride; reason = "chosen by you"
        } else if label.looksLikeSeries {
            kind = .tv; reason = "the disc label has season / volume markers"
        } else if playAllEpisodes >= 3 {
            kind = .tv; reason = "the disc menu plays \(playAllEpisodes) episodes in one title"
        } else if episodeLikeTitles(info?.titles ?? []).count >= 3 {
            kind = .tv; reason = "the disc has several titles of episode length"
        } else {
            kind = .movie; reason = "no series markers or episode-length titles"
        }
        return MediaIdentity(name: name, kind: kind, format: fmt, encrypted: encrypted, label: label, reason: reason)
    }

    /// Template values shared by every file of the job.
    func templateValues(rip: String) -> [String: String] {
        [
            "name": name,
            "kind": kind.rawValue,
            "format": formatCode,
            "rip": rip,
            "discLabel": label.setDescription,
            "discNumber": label.disc.map(String.init) ?? "",
            "season": label.season.map(String.init) ?? "",
            "part": label.part.map(String.init) ?? "",
            "volumeNumber": label.volume.map(String.init) ?? "",
            // Per-file values default to empty so conditional sections in templates drop out.
            "episode": "", "episodeNumber": "", "episodeTitle": "", "track": "",
        ]
    }

    /// "Title 11" for DVD titles, "Playlist 00800" for Blu-ray playlists, else MakeMKV's title number.
    static func trackLabel(_ t: TitleInfo) -> String {
        let src = t.sourceFileName
        if src.lowercased().hasSuffix(".mpls") { return "Playlist \((src as NSString).deletingPathExtension)" }
        if let id = t.sourceTitleId { return "Title \(id)" }
        return "Title \(t.index)"
    }

    static func episodeLabel(_ n: Int, width: Int) -> String {
        "Episode " + String(repeating: "0", count: max(0, width - String(n).count)) + String(n)
    }
}

/// Decides which post-processing steps apply to a disc: `matchName` is a case-insensitive regular expression
/// tested against the show / movie name and the disc label; `matchFormats` lists format codes (a trailing
/// `*` matches any code with that prefix, e.g. `BR*` = `BR` and `BRe`). Empty conditions match everything.
enum PluginMatcher {
    static func matches(_ step: PostProcessStep, name: String, discLabel: String, formatCode: String) -> Bool {
        let pattern = step.matchName.trimmingCharacters(in: .whitespaces)
        if !pattern.isEmpty {
            guard let re = try? NSRegularExpression(pattern: pattern, options: [.caseInsensitive]) else { return false }
            let hit = [name, discLabel].contains { s in re.firstMatch(in: s, range: NSRange(s.startIndex..., in: s)) != nil }
            if !hit { return false }
        }
        let formats = step.matchFormats.map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
        if formats.isEmpty { return true }
        return formats.contains { f in
            if f.hasSuffix("*") { return formatCode.lowercased().hasPrefix(f.dropLast().lowercased()) }
            return f.caseInsensitiveCompare(formatCode) == .orderedSame
        }
    }

    static func validate(_ pattern: String) -> String? {
        let p = pattern.trimmingCharacters(in: .whitespaces)
        if p.isEmpty { return nil }
        do { _ = try NSRegularExpression(pattern: p) } catch { return "Invalid regular expression" }
        return nil
    }
}
