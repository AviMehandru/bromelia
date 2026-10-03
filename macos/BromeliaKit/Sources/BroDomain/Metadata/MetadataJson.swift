import BroFoundation

/// Helpers shared by the TMDb and OMDb builders and parsers.
enum MetadataJson {
    /// RFC 3986 percent-encoding: everything but letters, digits and `-._~`, as UTF-8.
    static func escape(_ s: String) -> String {
        var out = ""
        for b in s.utf8 {
            switch b {
            case UInt8(ascii: "A")...UInt8(ascii: "Z"), UInt8(ascii: "a")...UInt8(ascii: "z"), UInt8(ascii: "0")...UInt8(ascii: "9"),
                 UInt8(ascii: "-"), UInt8(ascii: "."), UInt8(ascii: "_"), UInt8(ascii: "~"):
                out.unicodeScalars.append(Unicode.Scalar(b))
            default:
                let hex = Array("0123456789ABCDEF")
                out += "%" + String(hex[Int(b >> 4)]) + String(hex[Int(b & 0xF)])
            }
        }
        return out
    }

    static func object(_ bytes: [UInt8]) -> JsonValue? {
        guard let v = JsonValue.parse(bytes), case .object = v else { return nil }
        return v
    }

    static func objects(_ v: JsonValue?) -> [JsonValue] { (v?.array ?? []).filter { $0.members != nil } }

    /// A string other than OMDb's "N/A", else "".
    static func text(_ v: JsonValue?) -> String { v?.string.flatMap { $0 == "N/A" ? nil : $0 } ?? "" }

    /// The year at the start of a date or a span ("1994–2004").
    static func year(_ v: JsonValue?) -> Int? {
        guard let s = v?.string, s.utf8.count >= 4 else { return nil }
        let digits = Array(s.utf8.prefix(4))
        guard digits.allSatisfy({ (48...57).contains($0) }) else { return nil }
        return Int(String(decoding: digits, as: UTF8.self))
    }

    static func int(_ v: JsonValue?) -> Int? {
        guard case .integer(let n)? = v, n >= Int64(Int32.min), n <= Int64(Int32.max) else { return nil }
        return Int(n)
    }
}
