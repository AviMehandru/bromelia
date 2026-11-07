import BroFoundation
import CryptoKit
import Foundation

/// SHA256SUMS, in the format `sha256sum -c` and `shasum -a 256 -c` read.
public enum Sha256Sums {
    public static let fileName = "SHA256SUMS"

    static let line = try! NSRegularExpression(pattern: "^([0-9a-fA-F]{64}) [ *](.+)$")

    /// "hash  path" or "hash *path" lines; others are skipped. Hashes come back in lower case.
    public static func parse(_ text: String) -> [SumEntry] {
        lines(of: text).compactMap { raw in
            var l = raw
            if l.hasSuffix("\r") { l.removeLast() }
            guard let m = line.firstMatch(in: l, range: NSRange(l.startIndex..., in: l)),
                  let h = Range(m.range(at: 1), in: l), let p = Range(m.range(at: 2), in: l) else { return nil }
            return SumEntry(path: String(l[p]), sha256: String(l[h]).lowercased())
        }
    }

    /// "<sha256>  <path>" per line, sorted by path (code point order), with a final newline.
    public static func render(_ entries: [SumEntry]) -> String {
        entries.sorted { Array($0.path.utf8).lexicographicallyPrecedes(Array($1.path.utf8)) }.map { "\($0.sha256)  \($0.path)\n" }.joined()
    }

    /// The entries of `existing` (an earlier job's in the same folder) and `entries`; a path in both takes the new
    /// hash. Paths are the same only byte for byte: String's == is canonical, and two spellings can be two files. The file
    /// is rewritten from this, so a line that can't be read follows the entries as it is, byte for byte: it isn't
    /// Bromelia's to drop or to repair.
    public static func merge(_ existing: [UInt8], entries: [SumEntry]) -> [UInt8] {
        var merged: [[UInt8]: SumEntry] = [:]
        var unread: [UInt8] = []
        for var line in existing.split(separator: UInt8(ascii: "\n"), omittingEmptySubsequences: true) {
            if line.last == UInt8(ascii: "\r") { line.removeLast() }
            if line.isEmpty { continue }
            if let text = String(bytes: line, encoding: .utf8), let e = parse(text).first {
                merged[Array(e.path.utf8)] = e
            } else {
                unread += line + [UInt8(ascii: "\n")]
            }
        }
        for e in entries { merged[Array(e.path.utf8)] = e }
        return Array(render(Array(merged.values)).utf8) + unread
    }

    /// SHA-256 of bytes in memory, lower-case hex (a file is hashed as a stream with `Sha256Hasher`).
    public static func hash(_ bytes: [UInt8]) -> String {
        hex(SHA256.hash(data: bytes))
    }
}
