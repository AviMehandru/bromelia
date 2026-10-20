import BroDomain
import BroFoundation
import BroPorts

/// The host's MessageRenderer (plan §16): a message `{code, params, cause}` as text, from a catalog of ICU MessageFormat
/// texts. The subset shared/messages uses: `{x}`; plural with =N, one, other and #; select with other; the custom
/// formats bytes, duration and durationPrecise (codes.json's paramTypes). Params of type message, messages and
/// messageList are rendered from the same catalog, and a cause follows as " (cause)". Apostrophes follow ICU: '' is
/// one, and one before { or } (or # in a plural) quotes the text up to the next. It never fails: a missing param shows
/// as {name}, an unknown code as the code, and an unclosed brace as text.
public final class MessageTextRenderer: MessageRenderer {
    private typealias S = [Unicode.Scalar]
    private static let units = ["B", "KB", "MB", "GB", "TB"]

    private let catalog: MessageTexts

    public init(_ catalog: MessageTexts) {
        self.catalog = catalog
    }

    public func render(_ message: BroMessage) -> String { renderJson(message.toJson()) }

    /// The text of a message in its JSON form `{code, params, cause}`: a BroError's, or a nested message's.
    public func renderJson(_ message: JsonValue) -> String {
        let code = message["code"]?.string ?? ""
        let rendered = catalog.text(code).map { format(Array($0.unicodeScalars), message["params"], nil, code) } ?? code
        if let cause = message["cause"], !cause.isNull { return rendered + " (" + renderJson(cause) + ")" }
        return rendered
    }

    private func format(_ p: S, _ args: JsonValue?, _ hash: Int64?, _ code: String) -> String {
        var out = String.UnicodeScalarView()
        var i = 0
        while i < p.count {
            let c = p[i]
            if Self.quoteStarts(p, i, hash != nil) {
                i = Self.quoteEnd(p, i, &out)
            } else if c == "#", let h = hash {
                out.append(contentsOf: String(h).unicodeScalars)
                i += 1
            } else if c == "{" {
                guard let end = Self.matching(p, i) else {
                    out.append(contentsOf: p[i...])
                    break
                }
                out.append(contentsOf: argument(Array(p[(i + 1)..<end]), args, code).unicodeScalars)
                i = end + 1
            } else {
                out.append(c)
                i += 1
            }
        }
        return String(out)
    }

    // An apostrophe that quotes: before another, a brace, or (in a plural's text) a #.
    private static func quoteStarts(_ p: S, _ i: Int, _ hash: Bool) -> Bool {
        guard p[i] == "'", i + 1 < p.count else { return false }
        let n = p[i + 1]
        return n == "'" || n == "{" || n == "}" || (hash && n == "#")
    }

    // Past the quote at i, appending what it stands for to out: '' is one apostrophe; otherwise the text up to the next
    // lone apostrophe (or the end), in which '' is one too.
    private static func quoteEnd(_ p: S, _ i: Int, _ out: inout String.UnicodeScalarView) -> Int {
        if p[i + 1] == "'" {
            out.append("'")
            return i + 2
        }
        var j = i + 1
        while j < p.count {
            if p[j] == "'" {
                if j + 1 < p.count, p[j + 1] == "'" {
                    out.append("'")
                    j += 2
                    continue
                }
                return j + 1
            }
            out.append(p[j])
            j += 1
        }
        return j
    }

    private static func skipQuote(_ p: S, _ i: Int) -> Int {
        var ignored = String.UnicodeScalarView()
        return quoteEnd(p, i, &ignored)
    }

    // The brace that closes the one at open; nil when none does.
    private static func matching(_ p: S, _ open: Int) -> Int? {
        var depth = 0
        var i = open
        while i < p.count {
            if quoteStarts(p, i, false) {
                i = skipQuote(p, i)
                continue
            }
            if p[i] == "{" {
                depth += 1
            } else if p[i] == "}" {
                depth -= 1
                if depth == 0 { return i }
            }
            i += 1
        }
        return nil
    }

    private func argument(_ body: S, _ args: JsonValue?, _ code: String) -> String {
        let parts = Self.splitTop(body)
        let name = Self.trimmed(parts[0])
        let value = args?[name]
        let missing = value == nil || value!.isNull
        let type = parts.count > 1 ? Self.trimmed(parts[1]) : ""
        let style = parts.count > 2 ? parts[2] : []
        if type == "select" {
            let branches = Self.branches(style)
            let key = missing ? nil : plain(value!, "; ")
            return format(key.flatMap { branches[$0] } ?? branches["other"] ?? [], args, nil, code)
        }
        guard let value, !missing else { return "{" + name + "}" }
        switch type {
        case "":
            return plain(value, catalog.isMessageList(code, param: name) ? ", " : "; ")
        case "plural":
            let n = value.int ?? 0
            let branches = Self.branches(style)
            let text = branches["=\(n)"] ?? (n == 1 ? branches["one"] : nil) ?? branches["other"] ?? []
            return format(text, args, n, code)
        case "bytes": return Self.bytes(value.int ?? 0)
        case "duration": return Self.duration(value.double ?? 0)
        case "durationPrecise": return Self.durationPrecise(value.double ?? 0)
        default: return plain(value, "; ")
        }
    }

    // name [, type [, style]], split at the commas outside braces.
    private static func splitTop(_ s: S) -> [S] {
        var parts: [S] = []
        var depth = 0, start = 0, i = 0
        while i < s.count && parts.count < 2 {
            if s[i] == "{" {
                depth += 1
            } else if s[i] == "}" {
                depth -= 1
            } else if s[i] == "," && depth == 0 {
                parts.append(Array(s[start..<i]))
                start = i + 1
            }
            i += 1
        }
        parts.append(Array(s[start...]))
        return parts
    }

    // "key {text} key {text} …"; the first of a key counts, and an unclosed branch ends the list.
    private static func branches(_ s: S) -> [String: S] {
        var d: [String: S] = [:]
        var i = 0
        while i < s.count {
            guard let open = s[i...].firstIndex(of: "{"), let close = matching(s, open) else { break }
            let key = trimmed(Array(s[i..<open]))
            if d[key] == nil { d[key] = Array(s[(open + 1)..<close]) }
            i = close + 1
        }
        return d
    }

    private static func trimmed(_ s: S) -> String {
        func space(_ c: Unicode.Scalar) -> Bool { c == " " || c == "\n" || c == "\t" || c == "\r" }
        guard let first = s.firstIndex(where: { !space($0) }), let last = s.lastIndex(where: { !space($0) }) else { return "" }
        return String(String.UnicodeScalarView(s[first...last]))
    }

    private static func isMessage(_ v: JsonValue) -> Bool { v.members != nil && v["code"]?.string != nil }

    private func plain(_ v: JsonValue, _ messageJoin: String) -> String {
        switch v {
        case .string(let s): return s
        case .integer(let i): return String(i)
        case .bool(let b): return b ? "true" : "false"
        case .array(let items) where !items.isEmpty && items.allSatisfy(Self.isMessage):
            return items.map { renderJson($0) }.joined(separator: messageJoin)
        case .array(let items): return items.map { plain($0, "; ") }.joined(separator: ", ")
        case .null: return ""
        case _ where Self.isMessage(v): return renderJson(v)
        default: return v.description
        }
    }

    // Decimal units like a file manager: one decimal, rounded half up, the unit chosen after rounding (999950 is 1.0 MB).
    private static func bytes(_ n: Int64) -> String {
        if n < 1000 { return "\(n) B" }
        var u = 1
        var tenths: Int64
        while true {
            var q: Int64 = 100
            for _ in 1..<u { q *= 1000 }
            tenths = n / q + (n % q >= q / 2 ? 1 : 0)
            if tenths < 10000 || u == units.count - 1 { break }
            u += 1
        }
        return "\(tenths / 10).\(tenths % 10) \(units[u])"
    }

    private static func two(_ n: Int64) -> String { n < 10 ? "0\(n)" : "\(n)" }

    // A Double that converts to Int64 without trapping (nonsense such as NaN is 0).
    private static func whole(_ d: Double) -> Int64 { d.isFinite && abs(d) < 9e18 ? Int64(d) : 0 }

    // m:ss, or h:mm:ss from an hour; fractions of a second dropped.
    private static func duration(_ seconds: Double) -> String {
        let s = whole(seconds)
        return s >= 3600 ? "\(s / 3600):\(two(s / 60 % 60)):\(two(s % 60))" : "\(s / 60):\(two(s % 60))"
    }

    // h:mm:ss.sss, rounded to the millisecond.
    private static func durationPrecise(_ seconds: Double) -> String {
        let ms = whole((seconds * 1000).rounded())
        let frac = ms % 1000
        return "\(ms / 3_600_000):\(two(ms / 60000 % 60)):\(two(ms / 1000 % 60)).\(frac < 10 ? "00" : frac < 100 ? "0" : "")\(frac)"
    }
}
