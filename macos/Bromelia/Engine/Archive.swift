import Foundation
import CryptoKit

/// SHA-256 checksums of produced files, written as `SHA256SUMS` in the output folder in the format of
/// `sha256sum` / `shasum -a 256`, so a copy can be checked later with `shasum -a 256 -c SHA256SUMS`.
enum Checksums {
    static let fileName = "SHA256SUMS"

    struct Entry: Codable, Hashable, Sendable {
        /// Path relative to the output folder, with `/` separators.
        var path: String
        var size: Int64
        var sha256: String
    }

    /// Every regular file under `items` (folders are walked recursively), relative to `base`, sorted.
    static func files(_ items: [URL], base: URL) -> [(url: URL, relative: String)] {
        let fm = FileManager.default
        var out: [(URL, String)] = []
        let basePath = base.standardizedFileURL.path
        func rel(_ u: URL) -> String {
            let p = u.standardizedFileURL.path
            return p.hasPrefix(basePath + "/") ? String(p.dropFirst(basePath.count + 1)) : u.lastPathComponent
        }
        for item in items {
            var isDir: ObjCBool = false
            guard fm.fileExists(atPath: item.path, isDirectory: &isDir) else { continue }
            if isDir.boolValue {
                let e = fm.enumerator(at: item, includingPropertiesForKeys: [.isRegularFileKey], options: [.skipsHiddenFiles])
                while let u = e?.nextObject() as? URL {
                    if (try? u.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true { out.append((u, rel(u))) }
                }
            } else {
                out.append((item, rel(item)))
            }
        }
        var seen = Set<String>()
        return out.filter { seen.insert($0.1).inserted }.sorted { $0.1 < $1.1 }
    }

    /// Streams a file through SHA-256. `progress` receives the bytes read so far. Throws on read errors.
    static func sha256(of url: URL, progress: ((Int64) -> Void)? = nil, isCancelled: () -> Bool = { false }) throws -> String {
        let h = try FileHandle(forReadingFrom: url)
        defer { try? h.close() }
        var hasher = SHA256()
        var done: Int64 = 0
        while true {
            if isCancelled() { throw CancellationError() }
            let chunk = try autoreleasepool { try h.read(upToCount: 8 << 20) } ?? Data()
            if chunk.isEmpty { break }
            hasher.update(data: chunk)
            done += Int64(chunk.count)
            progress?(done)
        }
        return hasher.finalize().map { String(format: "%02x", $0) }.joined()
    }

    static func render(_ entries: [Entry]) -> String {
        entries.map { "\($0.sha256)  \($0.path)\n" }.joined()
    }

    /// Parses a SHA256SUMS file (`hash  path` or `hash *path`).
    static func parse(_ text: String) -> [(hash: String, path: String)] {
        text.split(whereSeparator: \.isNewline).compactMap { line in
            let s = String(line)
            guard s.count > 66, let space = s.firstIndex(of: " ") else { return nil }
            let hash = String(s[..<space])
            var rest = s[s.index(after: space)...]
            if rest.first == " " || rest.first == "*" { rest = rest.dropFirst() }
            return hash.count == 64 ? (hash.lowercased(), String(rest)) : nil
        }
    }

    /// Re-hashes the files listed in `folder/SHA256SUMS`. Returns the paths that are missing or differ.
    static func verify(folder: URL, isCancelled: () -> Bool = { false }) throws -> [String] {
        let text = try String(contentsOf: folder.appendingPathComponent(fileName), encoding: .utf8)
        var bad: [String] = []
        for (hash, path) in parse(text) {
            let url = folder.appendingPathComponent(path)
            guard FileManager.default.fileExists(atPath: url.path), (try? sha256(of: url, isCancelled: isCancelled)) == hash else {
                bad.append(path); continue
            }
        }
        return bad
    }
}

/// `bromelia.json`: a self-describing record of what was archived, kept next to the files.
struct ArchiveRecord: Codable, Sendable {
    struct Disc: Codable, Sendable {
        var label: String
        var volumeName: String
        var type: String
        var format: String
        var formatCode: String
        var encrypted: Bool
        var season: Int?
        var part: Int?
        var volume: Int?
        var disc: Int?
        /// The disc's fingerprint (DiscFingerprint), to recognise it when it is inserted again.
        var fingerprint: String?
    }
    struct Title: Codable, Sendable {
        var index: Int
        var sourceTitleId: Int?
        var sourceFile: String
        var duration: String
        var chapters: Int
        var sizeBytes: Int64
        var segmentMap: String
    }
    struct Episode: Codable, Sendable {
        var file: String
        var episode: Int?
        var sourceTitleId: Int
        var firstChapter: Int
        var lastChapter: Int
        /// The episode's title from the online lookup.
        var title: String?
    }

    var format = "bromelia-archive"
    var version = 2
    /// `success`, or `errors` when MakeMKV reported read errors (the files may be damaged).
    var status: String
    var name: String
    var kind: String
    var disc: Disc
    var rip: String
    var mode: String
    var source: String
    var driveName: String
    var makemkv: String
    var jobId: String
    var startedAt: Date?
    var finishedAt: Date?
    var titles: [Title]
    var episodes: [Episode]
    var files: [Checksums.Entry]
    var warnings: Int
    var errors: Int
    var errorMessages: [String]
    /// Errors MakeMKV reported while reading the disc for the rip or backup.
    var readErrors: [String]
    /// "Using LibreDrive mode (…)" details when the drive read the disc in LibreDrive mode.
    var libreDrive: String?
}

// MARK: - Already archived

/// Recognises a disc: "v1:" + 32 hex digits of SHA-256 over the volume name, the title count and, sorted, each
/// title's source title id, length in seconds, segment map and size in bytes. The same on every platform
/// (shared/fixtures/fingerprints.json).
enum DiscFingerprint {
    static func of(_ info: DiscInfo?) -> String? {
        guard let info, !info.titles.isEmpty else { return nil }
        let volume = info.volumeName.isEmpty ? info.name : info.volumeName
        let lines = info.titles.map { "\($0.sourceTitleId ?? -1)|\($0.durationSeconds)|\($0.segmentMap)|\($0.sizeBytes)" }
            .sorted { $0.utf8.lexicographicallyPrecedes($1.utf8) }
        var text = "bromelia-disc-fingerprint 1\nvolume:\(volume)\ntitles:\(info.titles.count)\n"
        for l in lines { text += l + "\n" }
        let hex = SHA256.hash(data: Data(text.utf8)).map { String(format: "%02x", $0) }.joined()
        return "v1:" + hex.prefix(32)
    }
}

/// Where a disc was archived before.
struct ArchivedMatch: Equatable, Sendable {
    var folder: String
    var archivedAt: Date?
}

/// A finished job that may have archived a disc (from the history).
struct ArchivedCandidate: Sendable {
    var fingerprint: String
    var folder: String
    var state: JobState
    var finishedAt: Date?
}

enum ArchiveLookup {
    /// The first candidate (newest first) that archived `fingerprint` successfully and whose folder still exists; else,
    /// with a root, the first bromelia*.json with status "success" and that fingerprint in root or up to four
    /// folders below it.
    static func find(fingerprint: String?, candidates: [ArchivedCandidate], root: URL?) -> ArchivedMatch? {
        guard let fingerprint, !fingerprint.isEmpty else { return nil }
        if let c = candidates.first(where: { $0.fingerprint == fingerprint && $0.state == .succeeded && isDirectory($0.folder) }) {
            return ArchivedMatch(folder: c.folder, archivedAt: c.finishedAt)
        }
        guard let root else { return nil }
        return scan(root, fingerprint: fingerprint, depth: 0)
    }

    static func isRecordName(_ name: String) -> Bool { name.hasPrefix("bromelia") && name.hasSuffix(".json") }

    private static func isDirectory(_ path: String) -> Bool {
        var isDir: ObjCBool = false
        return FileManager.default.fileExists(atPath: path, isDirectory: &isDir) && isDir.boolValue
    }

    private static func scan(_ dir: URL, fingerprint: String, depth: Int) -> ArchivedMatch? {
        let fm = FileManager.default
        guard let names = try? fm.contentsOfDirectory(atPath: dir.path) else { return nil }
        var subdirs: [URL] = []
        for name in names where !name.hasPrefix(".") {
            let url = dir.appendingPathComponent(name)
            let values = try? url.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey, .isRegularFileKey])
            if isRecordName(name), values?.isRegularFile == true {
                if let when = recordMatches(url, fingerprint: fingerprint) { return ArchivedMatch(folder: dir.path, archivedAt: when) }
            } else if depth < 4, values?.isDirectory == true, values?.isSymbolicLink != true {
                subdirs.append(url)
            }
        }
        for sub in subdirs.sorted(by: { $0.path.utf8.lexicographicallyPrecedes($1.path.utf8) }) {
            if let m = scan(sub, fingerprint: fingerprint, depth: depth + 1) { return m }
        }
        return nil
    }

    /// The archive time of a record with status "success" and this fingerprint (distantPast when it has none); nil otherwise.
    private static func recordMatches(_ url: URL, fingerprint: String) -> Date?? {
        guard let data = try? Data(contentsOf: url),
              let o = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              o["format"] as? String == "bromelia-archive", o["status"] as? String == "success",
              let disc = o["disc"] as? [String: Any], disc["fingerprint"] as? String == fingerprint else { return nil }
        return .some((o["finishedAt"] as? String).flatMap { ISO8601DateFormatter().date(from: $0) })
    }
}

// MARK: - Episode numbering across discs

/// Where a TV disc's episode numbering continues: after the last episode of the previous disc of its set (same show,
/// season, part and volume; disc number one lower), read from the archive records (bromelia*.json) of the output
/// folder, or, in a media server library, from the episode numbers already in the season folder.
enum EpisodeContinuation {
    struct Query: Sendable {
        /// The show's name (after the online lookup) and the title read from the disc label.
        var name: String
        var labelTitle: String
        var season: Int?
        var part: Int?
        var volume: Int?
        var disc: Int
    }

    struct Found: Equatable, Sendable {
        var lastEpisode: Int
        /// Where it was found, for the log.
        var source: String
    }

    /// An archived disc of a TV show: its place in the set and its highest episode number.
    struct Record: Equatable, Sendable {
        var name: String
        var labelTitle: String
        var season: Int?
        var part: Int?
        var volume: Int?
        var disc: Int?
        var lastEpisode: Int?
        var folder: String
    }

    /// `records` are read from `folders` (the history's) and from `root` and up to four folders below it. The season
    /// folder is used only when no record is found for the previous disc and none for a later disc (which would mean the
    /// discs were ripped out of order).
    static func find(_ q: Query, root: URL?, folders: [String], seasonFolder: URL?, season: Int) -> Found? {
        guard q.disc > 1 else { return nil }
        var records: [Record] = []
        var seen = Set<String>()
        func add(_ url: URL) {
            guard seen.insert(url.standardizedFileURL.path).inserted, let r = record(url) else { return }
            records.append(r)
        }
        for f in folders {
            let dir = URL(fileURLWithPath: f, isDirectory: true)
            for name in (try? FileManager.default.contentsOfDirectory(atPath: f)) ?? [] where ArchiveLookup.isRecordName(name) {
                add(dir.appendingPathComponent(name))
            }
        }
        if let root { walk(root, depth: 0, visit: add) }
        let same = records.filter { sameSet($0, q) }
        if let prev = same.filter({ $0.disc == q.disc - 1 }).compactMap({ r in r.lastEpisode.map { (r, $0) } }).max(by: { $0.1 < $1.1 }) {
            return Found(lastEpisode: prev.1, source: "disc \(q.disc - 1), archived in \(prev.0.folder)")
        }
        guard !same.contains(where: { ($0.disc ?? 0) > q.disc }), let seasonFolder,
              let last = highestEpisode(in: seasonFolder, season: season) else { return nil }
        return Found(lastEpisode: last, source: "the highest episode in \(seasonFolder.path)")
    }

    static func sameSet(_ r: Record, _ q: Query) -> Bool {
        let name = MetadataLookup.normalize(q.name), title = MetadataLookup.normalize(q.labelTitle)
        let sameShow = (!name.isEmpty && MetadataLookup.normalize(r.name) == name) || (!title.isEmpty && MetadataLookup.normalize(r.labelTitle) == title)
        return sameShow && r.season == q.season && r.part == q.part && r.volume == q.volume
    }

    /// A bromelia*.json of a TV show that was archived (status success or errors), or nil.
    static func record(_ url: URL) -> Record? {
        guard let data = try? Data(contentsOf: url), let o = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              o["format"] as? String == "bromelia-archive", ["success", "errors"].contains(o["status"] as? String ?? ""),
              o["kind"] as? String == "tv", let disc = o["disc"] as? [String: Any] else { return nil }
        let volumeName = disc["volumeName"] as? String ?? ""
        let label = LabelParser.parse(volumeName.isEmpty ? disc["label"] as? String ?? "" : volumeName)
        let episodes = (o["episodes"] as? [[String: Any]] ?? []).compactMap { $0["episode"] as? Int }
        return Record(name: o["name"] as? String ?? "", labelTitle: label.title, season: disc["season"] as? Int, part: disc["part"] as? Int,
                      volume: disc["volume"] as? Int, disc: disc["disc"] as? Int, lastEpisode: episodes.max(),
                      folder: url.deletingLastPathComponent().path)
    }

    private static func walk(_ dir: URL, depth: Int, visit: (URL) -> Void) {
        guard let names = try? FileManager.default.contentsOfDirectory(atPath: dir.path) else { return }
        for name in names.sorted() where !name.hasPrefix(".") {
            let url = dir.appendingPathComponent(name)
            let values = try? url.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey, .isRegularFileKey])
            if ArchiveLookup.isRecordName(name), values?.isRegularFile == true {
                visit(url)
            } else if depth < 4, values?.isDirectory == true, values?.isSymbolicLink != true {
                walk(url, depth: depth + 1, visit: visit)
            }
        }
    }

    /// The highest episode number of `season` in the names of the files in `folder` (`… S02E05 …`), or nil.
    static func highestEpisode(in folder: URL, season: Int) -> Int? {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: folder.path)) ?? []
        return names.filter { !$0.hasPrefix(".") }.compactMap { name -> Int? in
            guard let m = name.firstMatch(of: /[Ss](\d{1,3})[Ee](\d{1,4})/), Int(m.1) == season else { return nil }
            return Int(m.2)
        }.max()
    }
}

// MARK: - Verifying archives

enum ArchiveVerifier {
    struct FolderCheck: Sendable, Equatable {
        var folder: String
        /// Entries in SHA256SUMS.
        var files = 0
        var bytes: Int64 = 0
        var missing: [String] = []
        var changed: [String] = []
        var unreadable: [String] = []
        /// Files in the folder that SHA256SUMS doesn't list (not an error).
        var extra: [String] = []
        /// SHA256SUMS couldn't be read.
        var error: String?

        var ok: Bool { error == nil && missing.isEmpty && changed.isEmpty && unreadable.isEmpty }

        /// "12 file(s) OK" / "1 changed, 2 missing of 12 file(s); 1 not listed".
        var summary: String {
            if let error { return error }
            var s: String
            if ok {
                s = "\(files) file(s) OK"
            } else {
                var parts: [String] = []
                if !changed.isEmpty { parts.append("\(changed.count) changed") }
                if !unreadable.isEmpty { parts.append("\(unreadable.count) unreadable") }
                if !missing.isEmpty { parts.append("\(missing.count) missing") }
                s = parts.joined(separator: ", ") + " of \(files) file(s)"
            }
            if !extra.isEmpty { s += "; \(extra.count) not listed" }
            return s
        }
    }

    /// Folders holding a SHA256SUMS: `path` itself and every folder below it (hidden folders skipped), sorted.
    static func folders(under path: URL) -> [URL] {
        var out: [URL] = []
        func walk(_ dir: URL) {
            let fm = FileManager.default
            guard let names = try? fm.contentsOfDirectory(atPath: dir.path) else { return }
            if names.contains(Checksums.fileName) { out.append(dir) }
            for name in names where !name.hasPrefix(".") {
                let url = dir.appendingPathComponent(name)
                let v = try? url.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey])
                if v?.isDirectory == true && v?.isSymbolicLink != true { walk(url) }
            }
        }
        walk(path)
        return out.sorted { $0.path.utf8.lexicographicallyPrecedes($1.path.utf8) }
    }

    /// Files Bromelia writes next to the archived ones that SHA256SUMS doesn't list.
    static func isOwnFile(_ name: String) -> Bool {
        name == Checksums.fileName || ArchiveLookup.isRecordName(name) || (name.hasPrefix("bromelia-log") && name.hasSuffix(".txt"))
            || name == "INCOMPLETE.txt" || name == "READ ERRORS.txt"
    }

    /// Progress: bytes hashed so far of the total, the folder and the file being read. Return false to stop.
    typealias Progress = (_ done: Int64, _ total: Int64, _ folder: String?, _ file: String?) -> Bool

    /// Re-hashes every file of every folder (blocking; call off the main actor). Returns the folders finished and
    /// whether `progress` asked to stop.
    static func verify(folders: [URL], progress: Progress? = nil) -> (results: [FolderCheck], stopped: Bool) {
        let fm = FileManager.default
        var sums: [[String: String]?] = []
        var total: Int64 = 0
        for folder in folders {
            if let text = try? String(contentsOf: folder.appendingPathComponent(Checksums.fileName), encoding: .utf8) {
                var map: [String: String] = [:]
                for e in Checksums.parse(text) { map[e.path] = e.hash }
                sums.append(map)
                for path in map.keys {
                    total += ((try? fm.attributesOfItem(atPath: folder.appendingPathComponent(path).path))?[.size] as? NSNumber)?.int64Value ?? 0
                }
            } else {
                sums.append(nil)
            }
        }
        var stopped = !(progress?(0, total, nil, nil) ?? true)
        var done: Int64 = 0
        var results: [FolderCheck] = []
        for (i, folder) in folders.enumerated() where !stopped {
            var r = FolderCheck(folder: folder.path)
            guard let map = sums[i] else {
                r.error = "SHA256SUMS can't be read"
                results.append(r)
                continue
            }
            if map.isEmpty { r.error = "SHA256SUMS lists no files" }
            r.files = map.count
            for path in map.keys.sorted(by: { $0.utf8.lexicographicallyPrecedes($1.utf8) }) {
                let url = folder.appendingPathComponent(path)
                var isDir: ObjCBool = false
                guard fm.fileExists(atPath: url.path, isDirectory: &isDir), !isDir.boolValue else {
                    r.missing.append(path)
                    continue
                }
                let size = ((try? fm.attributesOfItem(atPath: url.path))?[.size] as? NSNumber)?.int64Value ?? 0
                let before = done
                do {
                    let hash = try Checksums.sha256(of: url, progress: { n in
                        if !(progress?(before + n, total, folder.path, path) ?? true) { stopped = true }
                    }, isCancelled: { stopped })
                    if hash != map[path] { r.changed.append(path) }
                } catch {
                    if !stopped { r.unreadable.append(path) }
                }
                done = before + size
                r.bytes += size
                if stopped { break }
            }
            if stopped { break }
            r.extra = extraFiles(folder, listed: Set(map.keys))
            results.append(r)
        }
        return (results, stopped)
    }

    /// Visible files under `folder` that `listed` doesn't have, skipping folders that are archives of their own.
    static func extraFiles(_ folder: URL, listed: Set<String>) -> [String] {
        let fm = FileManager.default
        var out: [String] = []
        func walk(_ rel: String) {
            let dir = rel.isEmpty ? folder : folder.appendingPathComponent(rel)
            let names = ((try? fm.contentsOfDirectory(atPath: dir.path)) ?? []).filter { !$0.hasPrefix(".") }
                .sorted { $0.utf8.lexicographicallyPrecedes($1.utf8) }
            for name in names {
                let r = rel.isEmpty ? name : rel + "/" + name
                let url = folder.appendingPathComponent(r)
                let v = try? url.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey])
                if v?.isDirectory == true {
                    if v?.isSymbolicLink != true && !fm.fileExists(atPath: url.appendingPathComponent(Checksums.fileName).path) { walk(r) }
                } else if !listed.contains(r) && !(rel.isEmpty && isOwnFile(name)) {
                    out.append(r)
                }
            }
        }
        walk("")
        return out
    }
}

/// When a folder was last verified (archive-checks.json in the data folder).
struct CheckRecord: Codable, Equatable, Sendable {
    var checkedAt: Date
    var ok: Bool
    var summary: String
}

enum CheckRecords {
    static var file: URL { Paths.appSupport.appendingPathComponent("archive-checks.json") }

    static func load(from url: URL = file) -> [String: CheckRecord] {
        guard let data = try? Data(contentsOf: url) else { return [:] }
        return (try? ConfigStore.decoder().decode([String: CheckRecord].self, from: data)) ?? [:]
    }

    static func save(_ records: [String: CheckRecord], to url: URL = file) {
        try? Paths.ensureDirectory(url.deletingLastPathComponent())
        try? ConfigStore.encoder().encode(records).write(to: url, options: .atomic)
    }
}

/// Checks a ripped MKV against the title in the disc listing, using `mkvmerge -J`.
enum RipVerifier {
    struct Probe: Equatable, Sendable {
        var durationSeconds: Double?
        var trackTypes: [String]
        var chapterCount: Int
    }

    struct Result: Equatable, Sendable {
        /// Reasons the file must not be trusted (wrong or truncated title, unreadable file).
        var problems: [String] = []
        /// Differences worth noting that don't make the file unusable.
        var notes: [String] = []
    }

    /// Allowed difference between the listed and the actual duration: 5 s or 0.5 %, whichever is larger.
    static func durationTolerance(_ expected: Double) -> Double { max(5, expected * 0.005) }

    /// Parses `mkvmerge -J` output. Returns nil when mkvmerge did not recognise the file.
    static func parse(json data: Data) -> Probe? {
        guard let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let container = obj["container"] as? [String: Any],
              (container["recognized"] as? Bool) != false else { return nil }
        let props = container["properties"] as? [String: Any]
        let duration = (props?["duration"] as? NSNumber).map { $0.doubleValue / 1e9 }
        let types = (obj["tracks"] as? [[String: Any]] ?? []).map { $0["type"] as? String ?? "" }
        let chapters = (obj["chapters"] as? [[String: Any]] ?? []).reduce(0) { $0 + (($1["num_entries"] as? Int) ?? 0) }
        return Probe(durationSeconds: duration, trackTypes: types, chapterCount: chapters)
    }

    static func check(_ p: Probe, against title: TitleInfo) -> Result {
        var r = Result()
        if p.trackTypes.isEmpty { r.problems.append("the file contains no tracks") }
        if !p.trackTypes.isEmpty && !p.trackTypes.contains("video") && title.tracks.contains(where: { $0.kind == .video }) {
            r.problems.append("the file contains no video track")
        }
        let expected = Double(title.durationSeconds)
        if expected > 0 {
            if let d = p.durationSeconds {
                if abs(d - expected) > durationTolerance(expected) {
                    r.problems.append("it lasts \(TitleInfo.formatDuration(Int(d.rounded()))), the disc listing says \(TitleInfo.formatDuration(Int(expected)))")
                }
            } else {
                r.problems.append("mkvmerge reports no duration")
            }
        }
        if !title.tracks.isEmpty && p.trackTypes.count > title.tracks.count {
            r.notes.append("\(p.trackTypes.count) tracks, the disc listing has \(title.tracks.count)")
        }
        // MakeMKV may add a chapter at 00:00 (profile option), so allow one extra.
        if title.chapterCount > 1 && (p.chapterCount < title.chapterCount || p.chapterCount > title.chapterCount + 1) {
            r.notes.append("\(p.chapterCount) chapters, the disc listing has \(title.chapterCount)")
        }
        return r
    }

    /// Runs `mkvmerge -J`. Returns nil when the file can't be read.
    static func probe(mkvmerge: URL, file: URL) async -> Probe? {
        let collector = LineCollector()
        let runner = ProcessRunner(executable: mkvmerge, arguments: ["-J", file.path])
        guard let out = try? await runner.run(timeout: 300, onLine: { collector.append($0) }), out.exitCode <= 1,
              let data = collector.joined.data(using: .utf8) else { return nil }
        return parse(json: data)
    }
}

/// Checks that a backup looks like a disc: a folder with a BDMV / VIDEO_TS / HVDVD_TS structure, or an ISO
/// image file (ISO 9660 or UDF volume descriptor at sector 16).
enum BackupVerifier {
    static func problem(_ url: URL, iso: Bool) -> String? {
        let fm = FileManager.default
        var isDir: ObjCBool = false
        guard fm.fileExists(atPath: url.path, isDirectory: &isDir) else { return "\(url.lastPathComponent) was not created" }
        if iso {
            if isDir.boolValue { return "\(url.lastPathComponent) is a folder, not an ISO image" }
            guard let h = try? FileHandle(forReadingFrom: url) else { return "\(url.lastPathComponent) can't be read" }
            defer { try? h.close() }
            guard (try? h.seek(toOffset: 32769)) != nil, let id = try? h.read(upToCount: 5),
                  ["CD001", "BEA01"].contains(String(decoding: id, as: UTF8.self)) else {
                return "\(url.lastPathComponent) is not an ISO / UDF image"
            }
            return nil
        }
        guard isDir.boolValue else { return "\(url.lastPathComponent) is not a folder" }
        let top = Set(((try? fm.contentsOfDirectory(atPath: url.path)) ?? []).map { $0.uppercased() })
        func has(_ dir: String, _ file: String) -> Bool {
            guard let real = ((try? fm.contentsOfDirectory(atPath: url.path)) ?? []).first(where: { $0.uppercased() == dir }) else { return false }
            let inner = (try? fm.contentsOfDirectory(atPath: url.appendingPathComponent(real).path)) ?? []
            return inner.contains { $0.uppercased() == file }
        }
        if top.contains("BDMV") { return has("BDMV", "INDEX.BDMV") ? nil : "BDMV/index.bdmv is missing" }
        if top.contains("VIDEO_TS") { return has("VIDEO_TS", "VIDEO_TS.IFO") ? nil : "VIDEO_TS/VIDEO_TS.IFO is missing" }
        if top.contains("HVDVD_TS") { return nil }
        return "it contains no BDMV, VIDEO_TS or HVDVD_TS folder"
    }
}

/// Free space on the destination volume.
enum DiskSpace {
    /// Bytes available to the user on the volume holding `url` (or its nearest existing parent).
    static func available(at url: URL) -> Int64? {
        var u = url
        while !FileManager.default.fileExists(atPath: u.path) && u.pathComponents.count > 1 { u = u.deletingLastPathComponent() }
        let v = try? u.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey, .volumeAvailableCapacityKey])
        if let important = v?.volumeAvailableCapacityForImportantUsage, important > 0 { return important }
        return v?.volumeAvailableCapacity.map(Int64.init)
    }

    /// `bytes` plus a margin: 2 % or 256 MB, whichever is larger (listing sizes are estimates).
    static func required(_ bytes: Int64) -> Int64 { bytes + max(256 << 20, bytes / 50) }
}
