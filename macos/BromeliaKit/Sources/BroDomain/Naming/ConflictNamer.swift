/// Names that don't collide.
public enum ConflictNamer {
    /// `name` when no entry of `existing` has it (ignoring ASCII case, as some file systems do), else "Name (2)",
    /// "Name (3)" … before the extension of a file.
    public static func next(_ name: String, existing: [String], isFolder: Bool = false) -> String {
        let taken = Set(existing.map(MessageCatalog.asciiLower))
        if !taken.contains(MessageCatalog.asciiLower(name)) { return name }
        var stem = name, ext = ""
        if !isFolder, let dot = name.lastIndex(of: "."), dot > name.startIndex {
            stem = String(name[..<dot])
            ext = String(name[dot...])
        }
        var n = 2
        while true {
            let candidate = "\(stem) (\(n))\(ext)"
            if !taken.contains(MessageCatalog.asciiLower(candidate)) { return candidate }
            n += 1
        }
    }
}
