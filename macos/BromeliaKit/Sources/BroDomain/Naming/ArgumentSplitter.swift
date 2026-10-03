/// Splits a command line like a POSIX shell (quotes and backslash escapes), without expansion. The same on every
/// platform, so configurations stay portable.
public enum ArgumentSplitter {
    public static func split(_ text: String) -> [String] {
        var args: [String] = []
        var cur = String.UnicodeScalarView()
        var single = false, dbl = false, has = false
        let s = Array(text.unicodeScalars)
        var i = 0
        while i < s.count {
            let c = s[i]
            if single {
                if c == "'" { single = false } else { cur.append(c) }
            } else if dbl {
                if c == "\"" {
                    dbl = false
                } else if c == "\\" && i + 1 < s.count {
                    i += 1
                    let n = s[i]
                    if n == "\"" || n == "\\" || n == "$" || n == "`" { cur.append(n) } else { cur.append(c); cur.append(n) }
                } else {
                    cur.append(c)
                }
            } else if c == "'" {
                single = true; has = true
            } else if c == "\"" {
                dbl = true; has = true
            } else if c == "\\" && i + 1 < s.count {
                // Keep Windows paths usable: a backslash only escapes quotes, spaces and backslashes.
                let n = s[i + 1]
                if n == "\"" || n == "'" || n == " " || n == "\\" { cur.append(n); i += 1 } else { cur.append(c) }
                has = true
            } else if c == " " || c == "\t" || c == "\n" {
                if has || !cur.isEmpty { args.append(String(cur)); cur = String.UnicodeScalarView(); has = false }
            } else {
                cur.append(c); has = true
            }
            i += 1
        }
        if has || !cur.isEmpty { args.append(String(cur)) }
        return args
    }

    /// An argument quoted for display, POSIX style on every platform: as is when it has only ASCII letters, digits
    /// and `-_./:=+,@%`, else in single quotes (`it's` → `'it'\''s'`).
    public static func quote(_ argument: String) -> String {
        let safe = !argument.isEmpty && argument.unicodeScalars.allSatisfy {
            ($0.isASCII && ($0.properties.isAlphabetic || ("0"..."9").contains($0))) || "-_./:=+,@%".unicodeScalars.contains($0)
        }
        if safe { return argument }
        return "'" + argument.split(separator: "'", omittingEmptySubsequences: false).joined(separator: "'\\''") + "'"
    }
}
