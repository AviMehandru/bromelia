import Foundation

/// MakeMKV's settings.conf format: `key = "value"` lines, # comments.
public enum SettingsConf {
    /// Every `key = value` line (quotes around the value removed); the last of a repeated key wins.
    public static func parse(_ text: String) -> [String: String] {
        var o: [String: String] = [:]
        for raw in text.split(separator: "\n", omittingEmptySubsequences: false) {
            let t = raw.trimmingCharacters(in: .whitespacesAndNewlines)
            guard !t.isEmpty, !t.hasPrefix("#"), let eq = t.firstIndex(of: "=") else { continue }
            let key = t[..<eq].trimmingCharacters(in: .whitespacesAndNewlines)
            var value = t[t.index(after: eq)...].trimmingCharacters(in: .whitespacesAndNewlines)
            if value.count >= 2 && value.hasPrefix("\"") && value.hasSuffix("\"") { value = String(value.dropFirst().dropLast()) }
            if !key.isEmpty { o[key] = value }
        }
        return o
    }

    /// A header naming `header`, then the keys in code point order; a double quote in a value becomes a single one and
    /// a line break a space.
    public static func render(_ settings: [String: String], header: String) -> String {
        var s = "#\n# MakeMKV settings file. \(header).\n# Changes made here are overwritten before every job.\n#\n\n"
        for key in settings.keys.sorted(by: { $0.unicodeScalars.lexicographicallyPrecedes($1.unicodeScalars) }) {
            let value = settings[key]!.replacingOccurrences(of: "\"", with: "'").replacingOccurrences(of: "\n", with: " ")
            s += "\(key) = \"\(value)\"\n"
        }
        return s
    }
}
