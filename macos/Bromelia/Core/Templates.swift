import Foundation

/// `{token}` templates used for output folders, file names and post-processing arguments.
///
/// * `{token}` inserts a value.
/// * `{token:3}` zero-pads numeric values to three digits.
/// * `{token?text}` inserts `text` (which may itself contain `{token}`) only when `token` is non-empty.
/// Unknown tokens are left untouched so typos are visible in the output.
enum TemplateRenderer {
    static let folderTokens: [(String, String)] = [
        ("name", "Movie or show name (inferred from the disc, or as entered)"),
        ("discLabel", "Place in the set, e.g. Season 2 Part 7 Disc 2 (empty when unknown)"),
        ("format", "Format code: DVD, BR, 4K; DVDe, BRe, 4Ke for backups that are not decrypted"),
        ("rip", "Rip or Backup"),
        ("kind", "movie or tv"),
        ("season", "Season number from the disc label"), ("discNumber", "Disc number from the disc label"),
        ("part", "Part number from the disc label"), ("volumeNumber", "Volume number from the disc label"),
        ("disc", "Disc name (falls back to the volume label)"),
        ("volume", "Volume label"),
        ("type", "dvd, bd, hddvd or disc"),
        ("drive", "Name of the drive configuration"),
        ("date", "Date, yyyy-MM-dd"),
        ("time", "Time, HH-mm-ss"),
        ("year", "Year"), ("month", "Month"), ("day", "Day"),
        ("job", "Short job identifier"),
    ]

    static let fileTokens: [(String, String)] = folderTokens + [
        ("episode", "Episode 138 (TV shows; empty for other titles)"),
        ("episodeNumber", "Episode number alone"),
        ("episodeTitle", "Episode title from the online lookup (TV shows; empty without one)"),
        ("track", "Source title: Title 11, Title 11 Ch 8-14 (split episodes) or Playlist 00800"),
        ("title", "Title name (or the disc name when the title has none)"),
        ("index", "MakeMKV title number (0-based)"),
        ("n", "Position of the title in this job (1-based)"),
        ("source", "Source title ID (playlist / VTS number)"),
        ("duration", "Duration, h-mm-ss"),
        ("chapters", "Chapter count"),
        ("original", "MakeMKV's original file name without extension"),
        ("comment", "Title comment reported by MakeMKV"),
    ]

    static let scriptTokens: [(String, String)] = fileTokens + [
        ("outputDir", "Job output folder"),
        ("file", "Current file (per-file steps) or first file"),
        ("filename", "Current file name (per-file steps)"),
        ("stem", "Current file name without extension (per-file steps)"),
        ("files", "All produced files, separated by spaces (quoted when split)"),
        ("status", "success, failed or cancelled"),
        ("manifest", "Path of the job manifest JSON"),
        ("device", "OS device path of the drive"),
        ("checksums", "Path of the SHA256SUMS file (empty when checksums are off)"),
    ]

    static func render(_ template: String, values: [String: String], sanitize: ((String) -> String)? = nil) -> String {
        var out = ""
        var i = template.startIndex
        while i < template.endIndex {
            let c = template[i]
            if c == "{", let close = matchingBrace(in: template, from: i) {
                let inner = String(template[template.index(after: i)..<close])
                out += expand(inner, values: values, sanitize: sanitize) ?? "{\(inner)}"
                i = template.index(after: close)
            } else {
                out.append(c)
                i = template.index(after: i)
            }
        }
        return out
    }

    private static func matchingBrace(in s: String, from open: String.Index) -> String.Index? {
        var depth = 0
        var i = open
        while i < s.endIndex {
            if s[i] == "{" { depth += 1 }
            if s[i] == "}" { depth -= 1; if depth == 0 { return i } }
            i = s.index(after: i)
        }
        return nil
    }

    private static func expand(_ inner: String, values: [String: String], sanitize: ((String) -> String)?) -> String? {
        if let q = inner.firstIndex(of: "?") {
            let key = String(inner[..<q])
            guard let v = values[key] else { return nil }
            return v.isEmpty ? "" : render(String(inner[inner.index(after: q)...]), values: values, sanitize: sanitize)
        }
        var key = inner
        var pad = 0
        if let colon = inner.firstIndex(of: ":") {
            key = String(inner[..<colon])
            pad = Int(inner[inner.index(after: colon)...]) ?? 0
        }
        guard var v = values[key] else { return nil }
        if pad > 0, let n = Int(v), n >= 0 {
            v = String(repeating: "0", count: max(0, pad - String(n).count)) + String(n)
        }
        return sanitize.map { $0(v) } ?? v
    }

    /// Makes a value safe to use as a single path component on every supported platform.
    static func sanitizeComponent(_ s: String) -> String {
        let forbidden = CharacterSet(charactersIn: "/\\:*?\"<>|").union(.controlCharacters)
        var r = String(s.unicodeScalars.map { forbidden.contains($0) ? "-" : Character($0) })
        r = r.trimmingCharacters(in: CharacterSet(charactersIn: " ."))
        return r
    }

    /// Renders a relative path template: token values are sanitised, `/` in the template separates folders.
    static func renderPath(_ template: String, values: [String: String]) -> String {
        let rendered = render(template, values: values, sanitize: sanitizeComponent)
        let parts = rendered.split(separator: "/").map { sanitizeComponent(String($0)) }.filter { !$0.isEmpty && $0 != ".." }
        return parts.joined(separator: "/")
    }

    static func dateValues(_ date: Date = Date()) -> [String: String] {
        let cal = Calendar.current
        let c = cal.dateComponents([.year, .month, .day, .hour, .minute, .second], from: date)
        func p(_ n: Int?) -> String { String(format: "%02d", n ?? 0) }
        return [
            "date": "\(c.year ?? 0)-\(p(c.month))-\(p(c.day))",
            "time": "\(p(c.hour))-\(p(c.minute))-\(p(c.second))",
            "year": "\(c.year ?? 0)", "month": p(c.month), "day": p(c.day),
        ]
    }
}

/// Splits a command line into arguments the way a POSIX shell would (quotes and backslashes),
/// without performing any expansion.
enum ArgumentSplitter {
    static func split(_ s: String) -> [String] {
        var args: [String] = []
        var cur = ""
        var inSingle = false, inDouble = false, hasToken = false
        var it = s.makeIterator()
        while let c = it.next() {
            if inSingle {
                if c == "'" { inSingle = false } else { cur.append(c) }
            } else if inDouble {
                if c == "\"" { inDouble = false }
                else if c == "\\", let n = it.next() {
                    if n == "\"" || n == "\\" || n == "$" || n == "`" { cur.append(n) } else { cur.append(c); cur.append(n) }
                } else { cur.append(c) }
            } else if c == "'" { inSingle = true; hasToken = true }
            else if c == "\"" { inDouble = true; hasToken = true }
            else if c == "\\", let n = it.next() { cur.append(n); hasToken = true }
            else if c == " " || c == "\t" || c == "\n" {
                if hasToken || !cur.isEmpty { args.append(cur); cur = ""; hasToken = false }
            } else { cur.append(c); hasToken = true }
        }
        if hasToken || !cur.isEmpty { args.append(cur) }
        return args
    }

    /// Quotes an argument for display in a copy-pasteable command line.
    static func quote(_ a: String) -> String {
        if !a.isEmpty && a.allSatisfy({ $0.isLetter || $0.isNumber || "-_./:=+,@%".contains($0) }) { return a }
        return "'" + a.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }
}
