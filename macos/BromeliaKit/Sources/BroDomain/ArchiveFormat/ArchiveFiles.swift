import BroFoundation
import CryptoKit
import Foundation

/// Which files of a unit's folder are Bromelia's own, which are media-server metadata, and which files the produced
/// items are.
public enum ArchiveFiles {
    static let ownLog = try! NSRegularExpression(pattern: "^(bromelia|makemkv)(-[0-9a-f]{8})?-(log|debug-log)([- (].*)?\\.txt$")

    /// Files Bromelia writes next to the archived ones that SHA256SUMS doesn't list: SHA256SUMS, the records
    /// (bromelia*.json), the logs (bromelia-log, makemkv-log, makemkv-debug-log; with a unit's short id in v3:
    /// bromelia-1a2b3c4d-log.txt …), and the INCOMPLETE / READ ERRORS notes.
    public static func isOwnFile(_ name: String) -> Bool {
        name == Sha256Sums.fileName || (name.hasPrefix("bromelia") && name.hasSuffix(".json"))
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
        name.hasPrefix(".") && (name.contains(".bromelia-tmp-") || name.contains(".part-"))
    }

    /// Files written for media servers, which SHA256SUMS doesn't list (servers may rewrite them): .nfo files and
    /// poster.jpg.
    public static func isMetadataFile(_ name: String) -> Bool {
        MessageCatalog.asciiLower(name).hasSuffix(".nfo") || name == "poster.jpg"
    }

    /// The files the produced items are: each produced file, and every file under a produced folder (from `tree`,
    /// the folder's files), relative paths in code point order, each once; hidden files (a component starting with a
    /// dot) are left out. Names are compared byte for byte (UTF-8), as C# and C do, never by Swift's canonical
    /// equivalence: an NFC and an NFD spelling of one name are two files (a network share can hold both).
    public static func expand(_ produced: [String], tree: [String]) -> [String] {
        var result: [[UInt8]: String] = [:]
        for p in produced {
            var item = Array(p.utf8)
            while item.last == UInt8(ascii: "/") { item.removeLast() }
            for f in tree where !f.split(separator: "/").contains(where: { $0.hasPrefix(".") }) {
                let bytes = Array(f.utf8)
                if bytes == item || bytes.starts(with: item + [UInt8(ascii: "/")]) { result[bytes] = f }
            }
        }
        return result.sorted { $0.key.lexicographicallyPrecedes($1.key) }.map(\.value)
    }
}
