import CryptoKit
import Foundation

/// SHA256SUMS, in the format `sha256sum -c` and `shasum -a 256 -c` read (I4).
public enum Sha256Sums {
    public static let fileName = "SHA256SUMS"

    static let line = try! NSRegularExpression(pattern: "^([0-9a-fA-F]{64}) [ *](.+)$")

    /// "hash  path" or "hash *path" lines; others are skipped. Hashes come back in lower case.
    public static func parse(_ text: String) -> [SumEntry] {
        text.split(separator: "\n", omittingEmptySubsequences: false).compactMap { raw in
            var l = String(raw)
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
    /// hash.
    public static func merge(_ existing: String, entries: [SumEntry]) -> String {
        var merged: [String: String] = [:]
        for e in parse(existing) { merged[e.path] = e.sha256 }
        for e in entries { merged[e.path] = e.sha256 }
        return render(merged.map { SumEntry(path: $0.key, sha256: $0.value) })
    }

    /// SHA-256 of bytes in memory, lower-case hex (files are hashed by the adapters, streaming).
    public static func hash(_ bytes: [UInt8]) -> String {
        SHA256.hash(data: bytes).map { b in
            let h = String(b, radix: 16)
            return h.count == 1 ? "0" + h : h
        }.joined()
    }
}
