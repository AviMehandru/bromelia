import BroFoundation

/// A unit's check: the verdict, the number of files SHA256SUMS lists, the files that changed, couldn't be read or
/// are missing, the files the folder has that SHA256SUMS doesn't list (not an error: post-processing may add
/// files), and the summary (check.ok / check.damaged, or the error: check.sumsEmpty).
public struct VerifyResult: Sendable, Equatable {
    public var result: CheckResult
    public var files: Int
    public var changed: [String]
    public var unreadable: [String]
    public var missing: [String]
    public var unlisted: [String]
    public var summary: BroMessage

    public init(result: CheckResult, files: Int, changed: [String], unreadable: [String], missing: [String], unlisted: [String], summary: BroMessage) {
        self.result = result
        self.files = files
        self.changed = changed
        self.unreadable = unreadable
        self.missing = missing
        self.unlisted = unlisted
        self.summary = summary
    }

    /// From the paths SHA256SUMS lists (`sums`), what hashing each found (`hashes`; a listed path without a verdict
    /// counts as missing), and the folder's files (relative paths, without nested archives: the adapter stops at
    /// folders with a SHA256SUMS of their own). Bromelia's own files at the top of the folder, media-server metadata
    /// and hidden files aren't unlisted.
    public static func compare(_ sums: [String], hashes: [String: FileVerdict], folderFiles: [String]) -> VerifyResult {
        func ordinal(_ a: String, _ b: String) -> Bool { Array(a.utf8).lexicographicallyPrecedes(Array(b.utf8)) }
        let listed = Set(sums).sorted(by: ordinal)
        if listed.isEmpty {
            return VerifyResult(result: .error, files: 0, changed: [], unreadable: [], missing: [], unlisted: [],
                                summary: BroMessage(.checkSumsEmpty, severity: .error))
        }
        func v(_ p: String) -> FileVerdict { hashes[p] ?? .missing }
        let changed = listed.filter { v($0) == .changed }
        let unreadable = listed.filter { v($0) == .unreadable }
        let missing = listed.filter { v($0) == .missing }
        let set = Set(listed)
        let unlisted = folderFiles.filter { f in
            let name = f.split(separator: "/", omittingEmptySubsequences: false).last.map(String.init) ?? f
            let top = !f.contains("/")
            return !set.contains(f) && !(top && ArchiveFiles.isOwnFile(name)) && !ArchiveFiles.isMetadataFile(name)
                && !f.split(separator: "/").contains(where: { $0.hasPrefix(".") })
        }.sorted(by: ordinal)
        let ok = changed.isEmpty && unreadable.isEmpty && missing.isEmpty
        let summary: BroMessage
        if ok {
            summary = BroMessage(.checkOk, [("files", .integer(Int64(listed.count))), ("unlisted", .integer(Int64(unlisted.count)))])
        } else {
            var parts: [JsonValue] = []
            if !changed.isEmpty { parts.append(BroMessage(.checkPartChanged, [("count", .integer(Int64(changed.count)))]).toJson()) }
            if !unreadable.isEmpty { parts.append(BroMessage(.checkPartUnreadable, [("count", .integer(Int64(unreadable.count)))]).toJson()) }
            if !missing.isEmpty { parts.append(BroMessage(.checkPartMissing, [("count", .integer(Int64(missing.count)))]).toJson()) }
            summary = BroMessage(.checkDamaged, [("parts", .array(parts)), ("files", .integer(Int64(listed.count))),
                                                 ("unlisted", .integer(Int64(unlisted.count)))], severity: .error)
        }
        return VerifyResult(result: ok ? .ok : .damaged, files: listed.count, changed: changed, unreadable: unreadable, missing: missing,
                            unlisted: unlisted, summary: summary)
    }
}
