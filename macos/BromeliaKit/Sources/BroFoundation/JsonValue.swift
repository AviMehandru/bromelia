/// A JSON value for params and records. Objects keep their keys in the order given, so a value written with
/// `encodeCanonical` comes out the same on every platform.
public enum JsonValue: Sendable, Equatable, CustomStringConvertible {
    case null
    case bool(Bool)
    /// A number written without a fraction or exponent that fits in 64 bits.
    case integer(Int64)
    /// Any other number.
    case number(Double)
    case string(String)
    case array([JsonValue])
    case object([(key: String, value: JsonValue)])

    public static func == (a: JsonValue, b: JsonValue) -> Bool {
        switch (a, b) {
        case (.null, .null): return true
        case let (.bool(x), .bool(y)): return x == y
        case let (.integer(x), .integer(y)): return x == y
        case let (.number(x), .number(y)): return x == y
        case let (.string(x), .string(y)): return x == y
        case let (.array(x), .array(y)): return x == y
        case let (.object(x), .object(y)):
            return x.count == y.count && zip(x, y).allSatisfy { $0.key == $1.key && $0.value == $1.value }
        default: return false
        }
    }

    // MARK: Access

    /// The member called `key` of an object; nil for anything else.
    public subscript(key: String) -> JsonValue? {
        if case .object(let members) = self { return members.last { $0.key == key }?.value }
        return nil
    }

    public var string: String? { if case .string(let s) = self { return s }; return nil }
    public var bool: Bool? { if case .bool(let b) = self { return b }; return nil }
    public var int: Int64? {
        switch self {
        case .integer(let i): return i
        case .number(let d) where d == d.rounded(.down) && abs(d) < 9.2e18: return Int64(d)
        default: return nil
        }
    }
    public var double: Double? {
        switch self {
        case .integer(let i): return Double(i)
        case .number(let d): return d
        default: return nil
        }
    }
    public var array: [JsonValue]? { if case .array(let a) = self { return a }; return nil }
    public var members: [(key: String, value: JsonValue)]? { if case .object(let m) = self { return m }; return nil }
    public var isNull: Bool { if case .null = self { return true }; return false }

    // MARK: Parsing

    /// Strict JSON (RFC 8259). Nil when the text isn't JSON. A repeated key keeps its first position and its
    /// last value. An integer outside the 64-bit range isn't accepted: as a double it would lose digits.
    public static func parse(_ text: String) -> JsonValue? { parse(Array(text.utf8)) }

    public static func parse(_ bytes: [UInt8]) -> JsonValue? {
        var p = Parser(bytes: bytes)
        if bytes.starts(with: [0xEF, 0xBB, 0xBF]) { p.i = 3 }
        p.skipSpace()
        guard let v = p.value(depth: 0) else { return nil }
        p.skipSpace()
        return p.i == bytes.count ? v : nil
    }

    private struct Parser {
        let bytes: [UInt8]
        var i = 0

        init(bytes: [UInt8]) { self.bytes = bytes }

        mutating func skipSpace() {
            while i < bytes.count, [0x20, 0x09, 0x0A, 0x0D].contains(bytes[i]) { i += 1 }
        }

        func peek() -> UInt8? { i < bytes.count ? bytes[i] : nil }

        mutating func literal(_ word: String) -> Bool {
            let w = Array(word.utf8)
            guard i + w.count <= bytes.count, Array(bytes[i..<i + w.count]) == w else { return false }
            i += w.count
            return true
        }

        mutating func value(depth: Int) -> JsonValue? {
            guard depth <= 512, let c = peek() else { return nil }
            switch c {
            case UInt8(ascii: "{"): return object(depth: depth)
            case UInt8(ascii: "["): return array(depth: depth)
            case UInt8(ascii: "\""): return string().map(JsonValue.string)
            case UInt8(ascii: "t"): return literal("true") ? .bool(true) : nil
            case UInt8(ascii: "f"): return literal("false") ? .bool(false) : nil
            case UInt8(ascii: "n"): return literal("null") ? .null : nil
            default: return number()
            }
        }

        mutating func object(depth: Int) -> JsonValue? {
            i += 1
            var members: [(key: String, value: JsonValue)] = []
            skipSpace()
            if peek() == UInt8(ascii: "}") { i += 1; return .object(members) }
            while true {
                skipSpace()
                guard peek() == UInt8(ascii: "\""), let key = string() else { return nil }
                skipSpace()
                guard literal(":") else { return nil }
                skipSpace()
                guard let v = value(depth: depth + 1) else { return nil }
                if let at = members.firstIndex(where: { $0.key == key }) { members[at].value = v } else { members.append((key, v)) }
                skipSpace()
                guard let c = peek() else { return nil }
                i += 1
                if c == UInt8(ascii: "}") { return .object(members) }
                if c != UInt8(ascii: ",") { return nil }
            }
        }

        mutating func array(depth: Int) -> JsonValue? {
            i += 1
            var items: [JsonValue] = []
            skipSpace()
            if peek() == UInt8(ascii: "]") { i += 1; return .array(items) }
            while true {
                skipSpace()
                guard let v = value(depth: depth + 1) else { return nil }
                items.append(v)
                skipSpace()
                guard let c = peek() else { return nil }
                i += 1
                if c == UInt8(ascii: "]") { return .array(items) }
                if c != UInt8(ascii: ",") { return nil }
            }
        }

        mutating func hex4() -> UInt32? {
            guard i + 4 <= bytes.count else { return nil }
            var v: UInt32 = 0
            for k in 0..<4 {
                let c = bytes[i + k]
                let d: UInt8
                switch c {
                case UInt8(ascii: "0")...UInt8(ascii: "9"): d = c - UInt8(ascii: "0")
                case UInt8(ascii: "a")...UInt8(ascii: "f"): d = c - UInt8(ascii: "a") + 10
                case UInt8(ascii: "A")...UInt8(ascii: "F"): d = c - UInt8(ascii: "A") + 10
                default: return nil
                }
                v = v * 16 + UInt32(d)
            }
            i += 4
            return v
        }

        mutating func string() -> String? {
            i += 1
            var out: [UInt8] = []
            while true {
                guard let c = peek() else { return nil }
                i += 1
                if c == UInt8(ascii: "\"") { break }
                if c < 0x20 { return nil }
                if c != UInt8(ascii: "\\") { out.append(c); continue }
                guard let e = peek() else { return nil }
                i += 1
                switch e {
                case UInt8(ascii: "\""): out.append(0x22)
                case UInt8(ascii: "\\"): out.append(0x5C)
                case UInt8(ascii: "/"): out.append(0x2F)
                case UInt8(ascii: "b"): out.append(0x08)
                case UInt8(ascii: "f"): out.append(0x0C)
                case UInt8(ascii: "n"): out.append(0x0A)
                case UInt8(ascii: "r"): out.append(0x0D)
                case UInt8(ascii: "t"): out.append(0x09)
                case UInt8(ascii: "u"):
                    guard var code = hex4() else { return nil }
                    if (0xD800..<0xDC00).contains(code), peek() == UInt8(ascii: "\\"), i + 1 < bytes.count, bytes[i + 1] == UInt8(ascii: "u") {
                        let save = i
                        i += 2
                        if let low = hex4(), (0xDC00..<0xE000).contains(low) {
                            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00)
                        } else {
                            i = save
                        }
                    }
                    let scalar = Unicode.Scalar(code) ?? "\u{FFFD}"
                    out.append(contentsOf: Array(String(Character(scalar)).utf8))
                default: return nil
                }
            }
            return String(decoding: out, as: UTF8.self)
        }

        mutating func number() -> JsonValue? {
            let start = i
            var integral = true
            func digit(_ c: UInt8?) -> Bool { c.map { $0 >= 0x30 && $0 <= 0x39 } ?? false }
            if peek() == UInt8(ascii: "-") { i += 1 }
            if peek() == UInt8(ascii: "0") {
                i += 1
            } else if let c = peek(), c >= UInt8(ascii: "1"), c <= UInt8(ascii: "9") {
                while digit(peek()) { i += 1 }
            } else {
                return nil
            }
            if peek() == UInt8(ascii: ".") {
                integral = false
                i += 1
                guard digit(peek()) else { return nil }
                while digit(peek()) { i += 1 }
            }
            if peek() == UInt8(ascii: "e") || peek() == UInt8(ascii: "E") {
                integral = false
                i += 1
                if peek() == UInt8(ascii: "+") || peek() == UInt8(ascii: "-") { i += 1 }
                guard digit(peek()) else { return nil }
                while digit(peek()) { i += 1 }
            }
            let text = String(decoding: bytes[start..<i], as: UTF8.self)
            // An integer that doesn't fit in 64 bits would lose digits as a double: not accepted.
            if integral { return Int64(text).map(JsonValue.integer) }
            return Double(text).map(JsonValue.number)
        }
    }

    // MARK: Writing

    /// UTF-8 JSON with two-space indentation, keys in the order given and a final newline: the same bytes as
    /// Python's `json.dumps(value, indent=2, ensure_ascii=False) + "\n"`.
    public static func encodeCanonical(_ value: JsonValue) -> [UInt8] {
        var out = ""
        write(&out, value, indent: 0)
        out += "\n"
        return Array(out.utf8)
    }

    /// `encodeCanonical` as text.
    public var description: String { String(decoding: JsonValue.encodeCanonical(self), as: UTF8.self) }

    private static func write(_ out: inout String, _ v: JsonValue, indent: Int) {
        switch v {
        case .null: out += "null"
        case .bool(let b): out += b ? "true" : "false"
        case .integer(let i): out += String(i)
        case .number(let d): out += formatNumber(d)
        case .string(let s): writeString(&out, s)
        case .array(let items):
            if items.isEmpty { out += "[]"; return }
            out += "["
            for (k, item) in items.enumerated() {
                out += (k == 0 ? "\n" : ",\n") + String(repeating: " ", count: indent + 2)
                write(&out, item, indent: indent + 2)
            }
            out += "\n" + String(repeating: " ", count: indent) + "]"
        case .object(let members):
            if members.isEmpty { out += "{}"; return }
            out += "{"
            for (k, m) in members.enumerated() {
                out += (k == 0 ? "\n" : ",\n") + String(repeating: " ", count: indent + 2)
                writeString(&out, m.key)
                out += ": "
                write(&out, m.value, indent: indent + 2)
            }
            out += "\n" + String(repeating: " ", count: indent) + "}"
        }
    }

    private static func writeString(_ out: inout String, _ s: String) {
        out += "\""
        for u in s.unicodeScalars {
            switch u {
            case "\"": out += "\\\""
            case "\\": out += "\\\\"
            case "\n": out += "\\n"
            case "\r": out += "\\r"
            case "\t": out += "\\t"
            case "\u{08}": out += "\\b"
            case "\u{0C}": out += "\\f"
            default:
                if u.value < 0x20 {
                    let h = String(u.value, radix: 16)
                    out += "\\u" + String(repeating: "0", count: 4 - h.count) + h
                } else {
                    out.unicodeScalars.append(u)
                }
            }
        }
        out += "\""
    }

    /// Python's float repr: the shortest digits that read back as the same double, in fixed notation for
    /// exponents from -4 to 15 and scientific notation otherwise.
    private static func formatNumber(_ d: Double) -> String {
        if d.isNaN { return "NaN" }
        if d.isInfinite { return d > 0 ? "Infinity" : "-Infinity" }
        if d == 0 { return d.sign == .minus ? "-0.0" : "0.0" }
        // Swift's description already has the shortest round-trip digits; take them and the exponent apart.
        let text = "\(abs(d))"
        let parts = text.split(separator: "e", maxSplits: 1)
        let mantissa = String(parts[0])
        var exponent = parts.count > 1 ? Int(String(parts[1].filter { $0 != "+" }))! : 0
        let dot = mantissa.firstIndex(of: ".")
        let intPart = dot.map { String(mantissa[..<$0]) } ?? mantissa
        let fracPart = dot.map { String(mantissa[mantissa.index(after: $0)...]) } ?? ""
        var digits = intPart + fracPart
        exponent += intPart.count - 1
        while digits.count > 1, digits.first == "0" { digits.removeFirst(); exponent -= 1 }
        while digits.count > 1, digits.last == "0" { digits.removeLast() }
        return (d < 0 ? "-" : "") + layOutNumber(digits, exponent)
    }

    static func layOutNumber(_ digits: String, _ exponent: Int) -> String {
        if exponent < -4 || exponent >= 16 {
            let m = digits.count == 1 ? digits : String(digits.prefix(1)) + "." + String(digits.dropFirst())
            let e = String(abs(exponent))
            return m + "e" + (exponent < 0 ? "-" : "+") + (e.count < 2 ? "0" + e : e)
        }
        if exponent < 0 { return "0." + String(repeating: "0", count: -exponent - 1) + digits }
        if digits.count <= exponent + 1 { return digits + String(repeating: "0", count: exponent + 1 - digits.count) + ".0" }
        return String(digits.prefix(exponent + 1)) + "." + String(digits.dropFirst(exponent + 1))
    }
}
