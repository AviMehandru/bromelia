/// makemkvcon's robot mode (`-r`): lines → events.
public enum Robot {
    /// Splits comma-separated fields; a quoted field may contain commas and `\"` / `\\` escapes.
    public static func splitFields(_ body: String) -> [String] {
        // Unicode scalars, not Characters: "\r\n" or a combining mark mustn't merge with a quote or comma.
        var fields: [String] = []
        var cur = String.UnicodeScalarView()
        var inQuotes = false, wasQuoted = false
        let chars = Array(body.unicodeScalars)
        var i = 0
        while i < chars.count {
            let c = chars[i]
            if inQuotes {
                if c == "\\" && i + 1 < chars.count {
                    let n = chars[i + 1]
                    if n == "\"" || n == "\\" { cur.append(n); i += 1 } else { cur.append(c) }
                } else if c == "\"" {
                    inQuotes = false
                } else {
                    cur.append(c)
                }
            } else if c == "," {
                fields.append(String(cur))
                cur = String.UnicodeScalarView()
                wasQuoted = false
            } else if c == "\"" && cur.isEmpty && !wasQuoted {
                inQuotes = true
                wasQuoted = true
            } else {
                cur.append(c)
            }
            i += 1
        }
        fields.append(String(cur))
        return fields
    }

    /// One line of output. Nil for an empty line; `.raw` for a line that isn't a robot record (or is one with
    /// fields missing).
    public static func parseLine(_ rawLine: String) -> RobotEvent? {
        var line = Substring(rawLine)
        while let last = line.unicodeScalars.last, last == "\r" || last == "\n" { line = Substring(line.unicodeScalars.dropLast()) }
        if line.isEmpty { return nil }
        let scalars = line.unicodeScalars
        guard let colon = scalars.firstIndex(of: ":") else { return .raw(text: String(line)) }
        let tag = String(scalars[..<colon])
        let body = String(scalars[scalars.index(after: colon)...])
        switch tag {
        case "MSG":
            let f = splitFields(body)
            if f.count >= 5, let code = int(f[0]), let flags = int(f[1]) {
                return .message(RobotMessage(code: code, flags: flags, count: int(f[2]) ?? 0, text: f[3], format: f[4], params: Array(f.dropFirst(5))))
            }
        case "PRGC", "PRGT":
            let f = splitFields(body)
            if f.count >= 3, let code = int(f[0]), let id = int(f[1]) {
                return tag == "PRGC" ? .progressCurrent(code: code, id: id, name: f[2]) : .progressTotal(code: code, id: id, name: f[2])
            }
        case "PRGV":
            let f = splitFields(body)
            if f.count >= 3, let a = int(f[0]), let b = int(f[1]), let m = int(f[2]) {
                return .progressValue(current: a, total: b, max: m)
            }
        case "DRV":
            let f = splitFields(body)
            if f.count >= 7, let index = int(f[0]), let state = int(f[1]), let flags = int(f[3]) {
                return .drive(index: index, state: state, flags: flags, identification: f[4], label: f[5], device: f[6])
            }
        case "TCOUNT":
            if let n = int(body) { return .titleCount(count: n) }
        case "CINFO":
            let f = splitFields(body)
            if f.count >= 3, let id = int(f[0]), let code = int(f[1]) { return .discInfo(id: id, code: code, value: f[2]) }
        case "TINFO":
            let f = splitFields(body)
            if f.count >= 4, let t = int(f[0]), let id = int(f[1]), let code = int(f[2]) {
                return .titleInfo(title: t, id: id, code: code, value: f[3])
            }
        case "SINFO":
            let f = splitFields(body)
            if f.count >= 5, let t = int(f[0]), let s = int(f[1]), let id = int(f[2]), let code = int(f[3]) {
                return .streamInfo(title: t, stream: s, id: id, code: code, value: f[4])
            }
        default:
            break
        }
        return .raw(text: String(line))
    }

    /// An integer field: optional sign and ASCII digits, surrounding spaces allowed.
    static func int(_ s: String) -> Int? {
        let t = s.drop { $0 == " " || $0 == "\t" }.reversed().drop { $0 == " " || $0 == "\t" }.reversed()
        let text = String(t)
        let digits = text.first == "-" || text.first == "+" ? text.dropFirst() : Substring(text)
        guard !digits.isEmpty, digits.allSatisfy({ $0.isASCII && $0.isNumber }) else { return nil }
        guard let v = Int(text.first == "+" ? String(digits) : text), v >= Int(Int32.min), v <= Int(Int32.max) else { return nil }
        return v
    }
}
