/// Safe file names.
public enum Sanitizer {
    /// A value made safe as one path component on every platform: / \ : * ? " < > | and control characters become
    /// `-`; spaces and dots are trimmed from both ends.
    public static func component(_ text: String) -> String {
        var out = String.UnicodeScalarView()
        for u in text.unicodeScalars {
            let bad = "/\\:*?\"<>|".unicodeScalars.contains(u) || u.value < 0x20 || (0x7F...0x9F).contains(u.value)
            out.append(bad ? "-" : u)
        }
        let s = String(out)
        let trim: (Character) -> Bool = { $0 == " " || $0 == "." }
        return String(s.drop(while: trim).reversed().drop(while: trim).reversed())
    }
}
