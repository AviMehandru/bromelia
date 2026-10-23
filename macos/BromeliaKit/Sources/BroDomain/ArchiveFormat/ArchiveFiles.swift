import BroFoundation
import CryptoKit
import Foundation

/// Which files of a unit's folder are Bromelia's own, which are media-server metadata, and which files the produced
/// items are.
public enum ArchiveFiles {
    static let ownLog = try! NSRegularExpression(pattern: "^(bromelia|makemkv)(-[0-9a-f]{8})?-(log|debug-log)([- (].*)?\\.txt$")

    /// Files Bromelia writes next to the archived ones that SHA256SUMS doesn't list: SHA256SUMS, the records
    /// (bromelia*.json), the logs (bromelia-log, makemkv-log, makemkv-debug-log; with a unit's short id in v3:
    /// bromelia-1a2b3c4d-log.txt …), and the INCOMPLETE / READ ERRORS notes. Names are bytes here and below, as in C#
    /// (ordinal) and C, never Swift's graphemes: a combining or prepended mark next to a '.' or '/' changes nothing.
    public static func isOwnFile(_ name: String) -> Bool {
        name == Sha256Sums.fileName || (name.utf8.starts(with: "bromelia".utf8) && endsWith(name, ".json"))
            || ownLog.firstMatch(in: name, range: NSRange(name.startIndex..., in: name)) != nil
            || name == "INCOMPLETE.txt" || name == "READ ERRORS.txt"
    }

    /// The unit a job archives: a UUID (version 8, RFC 9562) made from the first 16 bytes of SHA-256 of
    /// "bromelia-unit\n<jobId>", so a rerun of Seal names the same files (bromelia-<unit8>.json) without any randomness.
    public static func unitId(_ jobId: Id) -> Id {
        var b = Array(SHA256.hash(data: Array("bromelia-unit\n\(jobId.value)".utf8)).prefix(16))
        b[6] = (b[6] & 0x0F) | 0x80
        b[8] = (b[8] & 0x3F) | 0x80
        let hex = b.map { v in
            let h = String(v, radix: 16)
            return h.count == 1 ? "0" + h : h
        }.joined()
        let c = Array(hex)
        return Id([String(c[0..<8]), String(c[8..<12]), String(c[12..<16]), String(c[16..<20]), String(c[20..<32])].joined(separator: "-"))
    }

    /// What a write that was stopped leaves behind: .<name>.bromelia-tmp-<id> (FileSystem.writeAtomically) and
    /// .<name>.part-<id> (DataImager). Startup recovery removes them (review L3-3).
    public static func isLeftover(_ name: String) -> Bool {
        let bytes = Array(name.utf8)
        return bytes.first == dot && (bytes.firstRange(of: Array(".bromelia-tmp-".utf8)) != nil || bytes.firstRange(of: Array(".part-".utf8)) != nil)
    }

    /// Files written for media servers, which SHA256SUMS doesn't list (servers may rewrite them): .nfo files and
    /// poster.jpg.
    public static func isMetadataFile(_ name: String) -> Bool {
        endsWith(MessageCatalog.asciiLower(name), ".nfo") || name == "poster.jpg"
    }

    // Path helpers for Swift only (package: BroEngine's SealStep uses them; C# and C get the same from ordinal / byte
    // string functions).

    /// Whether a relative path has a hidden component: one whose first byte is '.' (".\u{301}note" too).
    package static func isHidden(_ path: some StringProtocol) -> Bool {
        path.utf8.split(separator: slash).contains { $0.first == dot }
    }

    /// Whether a relative path is a folder: it ends in '/' (FileTree's empty folders).
    package static func isFolder(_ path: String) -> Bool { path.utf8.last == slash }

    /// Whether a relative path is at the top of its folder: it holds no '/'.
    package static func isTop(_ path: String) -> Bool { !path.utf8.contains(slash) }

    /// The components of a relative path, split at each '/' (empty ones kept).
    package static func components(_ path: String) -> [String] {
        path.utf8.split(separator: slash, omittingEmptySubsequences: false).map { String(decoding: $0, as: UTF8.self) }
    }

    static let dot = UInt8(ascii: "."), slash = UInt8(ascii: "/")

    static func endsWith(_ name: String, _ suffix: String) -> Bool { name.utf8.reversed().starts(with: suffix.utf8.reversed()) }

    /// The files the produced items are: each produced file, and every file under a produced folder (from `tree`,
    /// the folder's files), relative paths in code point order, each once; hidden files (a component starting with a
    /// dot) are left out. Names are compared byte for byte (UTF-8), as C# and C do, never by Swift's canonical
    /// equivalence: an NFC and an NFD spelling of one name are two files (a network share can hold both).
    public static func expand(_ produced: [String], tree: [String]) -> [String] {
        var result: [[UInt8]: String] = [:]
        for p in produced {
            var item = Array(p.utf8)
            while item.last == UInt8(ascii: "/") { item.removeLast() }
            for f in tree where !isHidden(f) {
                let bytes = Array(f.utf8)
                if bytes == item || bytes.starts(with: item + [UInt8(ascii: "/")]) { result[bytes] = f }
            }
        }
        return result.sorted { $0.key.lexicographicallyPrecedes($1.key) }.map(\.value)
    }
}
