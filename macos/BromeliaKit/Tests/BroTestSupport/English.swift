import BroFoundation

/// Renders a message `{code, params}` in English from shared/messages/en.json, for fixtures that check a message's
/// text. The subset of ICU MessageFormat en.json uses: `{x}`, plural (with =N, one, other and #), select, bytes,
/// duration, durationPrecise, and nested messages (params of type message / messages). Apostrophes are plain text.
/// (The engine host's MessageRenderer, in Adapters, will do this properly.)
public enum English {
    nonisolated(unsafe) static var texts: JsonValue?
    nonisolated(unsafe) static var codes: JsonValue?

    public static func render(_ message: JsonValue) throws -> String {
        if texts == nil { texts = try Fixtures.sharedJson("messages/en.json") }
        if codes == nil { codes = try Fixtures.sharedJson("messages/codes.json")["codes"] }
        let code = message["code"]?.string ?? ""
        let text = texts?[code]?.string ?? code
        let rendered = try format(Array(text.unicodeScalars), message["params"] ?? .object([]), nil, code)
        if let cause = message["cause"], !cause.isNull { return rendered + " (" + (try render(cause)) + ")" }
        return rendered
    }

    private typealias S = [Unicode.Scalar]

    private static func format(_ p: S, _ args: JsonValue, _ hash: Int64?, _ code: String) throws -> String {
        var out = String.UnicodeScalarView()
        var i = 0
        while i < p.count {
            let c = p[i]
            if c == "#", let h = hash { out.append(contentsOf: String(h).unicodeScalars); i += 1; continue }
            if c != "{" { out.append(c); i += 1; continue }
            let end = try matching(p, i)
            out.append(contentsOf: try argument(Array(p[(i + 1)..<end]), args, code).unicodeScalars)
            i = end + 1
        }
        return String(out)
    }

    private static func matching(_ s: S, _ open: Int) throws -> Int {
        var depth = 0
        for i in open..<s.count {
            if s[i] == "{" { depth += 1 } else if s[i] == "}" { depth -= 1; if depth == 0 { return i } }
        }
        throw FixtureError("unbalanced braces")
    }

    private static func argument(_ body: S, _ args: JsonValue, _ code: String) throws -> String {
        let parts = splitTop(body, 3)
        let name = String(String.UnicodeScalarView(parts[0])).trimmingSpaces
        let value = args[name]
        // messages are joined with "; ", messageList with ", " (codes.json's paramTypes).
        if parts.count == 1 { return plain(value, codes?[code]?["params"]?[name]?.string == "messageList" ? ", " : "; ") }
        switch String(String.UnicodeScalarView(parts[1])).trimmingSpaces {
        case "plural":
            let n = value?.int ?? 0
            let b = try branches(parts[2])
            let text = b["=\(n)"] ?? (n == 1 ? b["one"] : nil) ?? b["other"] ?? []
            return try format(text, args, n, code)
        case "select":
            let key: String
            if case .bool(let v)? = value { key = v ? "true" : "false" } else { key = plain(value) }
            let b = try branches(parts[2])
            return try format(b[key] ?? b["other"] ?? [], args, nil, code)
        case "bytes": return bytes(value?.int ?? 0)
        case "duration", "durationPrecise": return clock(Int64(value?.double ?? 0))
        default: return plain(value)
        }
    }

    private static func plain(_ v: JsonValue?, _ messageJoin: String = "; ") -> String {
        switch v {
        case nil, .null?: return ""
        case .string(let s)?: return s
        case .integer(let i)?: return String(i)
        case .bool(let b)?: return b ? "true" : "false"
        case .array(let items)?:
            if items.allSatisfy({ $0["code"] != nil }) { return items.map { (try? render($0)) ?? "" }.joined(separator: messageJoin) }
            return items.map { plain($0) }.joined(separator: ", ")
        case let .object(m)? where m.contains(where: { $0.key == "code" }): return (try? render(v!)) ?? ""
        case let other?: return other.description
        }
    }

    private static func splitTop(_ s: S, _ max: Int) -> [S] {
        var parts: [S] = []
        var depth = 0, start = 0
        var i = 0
        while i < s.count && parts.count < max - 1 {
            if s[i] == "{" { depth += 1 } else if s[i] == "}" { depth -= 1 } else if s[i] == "," && depth == 0 {
                parts.append(Array(s[start..<i]))
                start = i + 1
            }
            i += 1
        }
        parts.append(Array(s[start...]))
        return parts
    }

    private static func branches(_ s: S) throws -> [String: S] {
        var d: [String: S] = [:]
        var i = 0
        while i < s.count {
            while i < s.count, s[i] == " " || s[i] == "\n" || s[i] == "\t" { i += 1 }
            if i >= s.count { break }
            guard let open = s[i...].firstIndex(of: "{") else { break }
            let key = String(String.UnicodeScalarView(s[i..<open])).trimmingSpaces
            let close = try matching(s, open)
            d[key] = Array(s[(open + 1)..<close])
            i = close + 1
        }
        return d
    }

    private static func bytes(_ n: Int64) -> String {
        let units = ["B", "KB", "MB", "GB", "TB"]
        var v = Double(n)
        var u = 0
        while v >= 1000 && u < units.count - 1 { v /= 1000; u += 1 }
        if u == 0 { return "\(n) B" }
        let tenths = Int64((v * 10).rounded())
        return "\(tenths / 10).\(tenths % 10) \(units[u])"
    }

    private static func clock(_ s: Int64) -> String {
        func two(_ n: Int64) -> String { n < 10 ? "0\(n)" : "\(n)" }
        return s >= 3600 ? "\(s / 3600):\(two(s / 60 % 60)):\(two(s % 60))" : "\(s / 60):\(two(s % 60))"
    }
}

extension String {
    var trimmingSpaces: String {
        String(drop { $0 == " " || $0 == "\n" }.reversed().drop { $0 == " " || $0 == "\n" }.reversed())
    }
}
