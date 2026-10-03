import Foundation

/// Which files of a unit's folder are Bromelia's own, which are media-server metadata, and which files the produced
/// items are.
public enum ArchiveFiles {
    static let ownLog = try! NSRegularExpression(pattern: "^(bromelia|makemkv)(-[0-9a-f]{8})?-(log|debug-log)([- (].*)?\\.txt$")

    /// Files Bromelia writes next to the archived ones that SHA256SUMS doesn't list: SHA256SUMS, the records
    /// (bromelia*.json), the logs (bromelia-log, makemkv-log, makemkv-debug-log; with a unit's short id in v3:
    /// bromelia-1a2b3c4d-log.txt …), and the INCOMPLETE / READ ERRORS notes.
    public static func isOwnFile(_ name: String) -> Bool {
        name == Sha256Sums.fileName || (name.hasPrefix("bromelia") && name.hasSuffix(".json"))
            || ownLog.firstMatch(in: name, range: NSRange(name.startIndex..., in: name)) != nil
            || name == "INCOMPLETE.txt" || name == "READ ERRORS.txt"
    }

    /// Files written for media servers, which SHA256SUMS doesn't list (servers may rewrite them): .nfo files and
    /// poster.jpg.
    public static func isMetadataFile(_ name: String) -> Bool {
        MessageCatalog.asciiLower(name).hasSuffix(".nfo") || name == "poster.jpg"
    }

    /// The files the produced items are: each produced file, and every file under a produced folder (from `tree`,
    /// the folder's files), relative paths in code point order, each once; hidden files (a component starting with a
    /// dot) are left out.
    public static func expand(_ produced: [String], tree: [String]) -> [String] {
        var result = Set<String>()
        for p in produced {
            var item = p
            while item.hasSuffix("/") { item.removeLast() }
            for f in tree where (f == item || f.hasPrefix(item + "/")) && !f.split(separator: "/").contains(where: { $0.hasPrefix(".") }) {
                result.insert(f)
            }
        }
        return result.sorted { Array($0.utf8).lexicographicallyPrecedes(Array($1.utf8)) }
    }
}
