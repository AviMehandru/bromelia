/// Names that don't collide.
public enum ConflictNamer {
    /// Whether two names are one name here: the same but for ASCII case, as some file systems have it.
    public static func same(_ a: String, _ b: String) -> Bool { key(a) == key(b) }

    /// `name` when no entry of `existing` has it (ignoring ASCII case, as some file systems do), else "Name (2)",
    /// "Name (3)" … before the extension of a file.
    public static func next(_ name: String, existing: [String], isFolder: Bool = false) -> String {
        let taken = Set(existing.map(key))
        if !taken.contains(key(name)) { return name }
        var stem = name, ext = ""
        let bytes = Array(name.utf8)
        if !isFolder, let dot = bytes.lastIndex(of: UInt8(ascii: ".")), dot > 0 {
            stem = String(decoding: bytes[..<dot], as: UTF8.self)
            ext = String(decoding: bytes[dot...], as: UTF8.self)
        }
        var n = 2
        while true {
            let candidate = "\(stem) (\(n))\(ext)"
            if !taken.contains(key(candidate)) { return candidate }
            n += 1
        }
    }

    /// By bytes: file systems compare what they store, and String's == would call two spellings of é one name.
    private static func key(_ name: String) -> [UInt8] { name.utf8.map { $0 >= 65 && $0 <= 90 ? $0 + 32 : $0 } }
}
