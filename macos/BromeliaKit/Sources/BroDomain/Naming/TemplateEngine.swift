/// `{token}` templates for folders, file names and script arguments. `{n:3}` zero-pads, `{token?text}` inserts text
/// only when the token is set and not empty, other unknown tokens stay as they are.
public enum TemplateEngine {
    public static func render(_ template: String, values: [String: String]) -> String { render(template, values, nil) }

    /// A relative path: values are made safe, `/` (or `\`) in the template separates folders, every component is
    /// made safe, and empty, `.` and `..` components are dropped. Folders are separated by `/` on every platform.
    public static func renderPath(_ template: String, values: [String: String]) -> String {
        let rendered = render(String(template.map { $0 == "\\" ? "/" : $0 }), values, Sanitizer.component)
        return rendered.split(separator: "/", omittingEmptySubsequences: false).map { Sanitizer.component(String($0)) }
            .filter { !$0.isEmpty && $0 != ".." }.joined(separator: "/")
    }

    private static func render(_ template: String, _ values: [String: String], _ sanitize: ((String) -> String)?) -> String {
        let s = Array(template.unicodeScalars)
        var out = String.UnicodeScalarView()
        var i = 0
        while i < s.count {
            if s[i] == "{", let close = matchingBrace(s, i) {
                let inner = String(String.UnicodeScalarView(s[(i + 1)..<close]))
                if let v = expand(inner, values, sanitize) { out.append(contentsOf: v.unicodeScalars) } else {
                    out.append(contentsOf: ("{" + inner + "}").unicodeScalars)
                }
                i = close + 1
                continue
            }
            out.append(s[i])
            i += 1
        }
        return String(out)
    }

    private static func matchingBrace(_ s: [Unicode.Scalar], _ open: Int) -> Int? {
        var depth = 0
        for i in open..<s.count {
            if s[i] == "{" { depth += 1 }
            if s[i] == "}" { depth -= 1; if depth == 0 { return i } }
        }
        return nil
    }

    private static func expand(_ inner: String, _ values: [String: String], _ sanitize: ((String) -> String)?) -> String? {
        if let q = inner.firstIndex(of: "?") {
            // A condition on a token that isn't set is false.
            guard let cond = values[String(inner[..<q])], !cond.isEmpty else { return "" }
            return render(String(inner[inner.index(after: q)...]), values, sanitize)
        }
        var key = inner
        var pad = 0
        if let colon = inner.firstIndex(of: ":") {
            key = String(inner[..<colon])
            pad = Robot.int(String(inner[inner.index(after: colon)...])) ?? 0
        }
        guard var v = values[key] else { return nil }
        if pad > 0, !v.isEmpty, v.allSatisfy({ $0.isASCII && $0.isNumber }), let n = Int(v) {
            let digits = String(n)
            v = String(repeating: "0", count: max(0, pad - digits.count)) + digits
        }
        return sanitize?(v) ?? v
    }
}
