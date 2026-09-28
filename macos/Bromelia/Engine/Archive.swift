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
    var version = 1
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
}
