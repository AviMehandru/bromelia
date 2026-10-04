/// Where a file or folder the user opened leads: MakeMKV opens discs (an image, or a folder holding BDMV, VIDEO_TS or
/// HVDVD_TS), not single files, so a file inside a disc structure opens the disc it belongs to.
public enum SourceResolver {
    static func isSeparator(_ c: Character) -> Bool { c == "/" || c == "\\" }

    static func parent(_ p: String) -> String {
        guard let i = p.lastIndex(where: isSeparator) else { return "" }
        if i == p.startIndex { return p.count > 1 ? String(p[i]) : "" }
        return String(p[..<i])
    }

    static func name(_ p: String) -> String { p.lastIndex(where: isSeparator).map { String(p[p.index(after: $0)...]) } ?? p }

    /// An image (.iso, .img, .udf), else the folder holding BDMV / VIDEO_TS / HVDVD_TS, looking from the item itself up
    /// to three levels, else the item as a folder.
    public static func source(_ path: String, isDirectory: Bool) -> MakemkvSource {
        var p = Substring(path)
        while let last = p.last, isSeparator(last) { p = p.dropLast() }
        let item = String(p)
        let lower = MessageCatalog.asciiLower(name(item))
        if !isDirectory && (lower.hasSuffix(".iso") || lower.hasSuffix(".img") || lower.hasSuffix(".udf")) { return .iso(path: item) }
        var dir = isDirectory ? item : parent(item)
        for _ in 0..<4 where !dir.isEmpty {
            let upper = name(dir).uppercased()
            if ["BDMV", "VIDEO_TS", "HVDVD_TS"].contains(upper) {
                let root = parent(dir)
                if !root.isEmpty { return .file(path: root) }
            }
            dir = parent(dir)
            if dir == "/" || dir == "\\" { break }
        }
        return .file(path: item)
    }
}
