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
