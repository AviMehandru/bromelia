import BroFoundation

/// Matches the titles of one listing to another: the listing a disc was opened with and the listing read when the
/// job starts, or a disc and its backup. MakeMKV numbers titles by position, which changes with the minimum length
/// setting, so titles are matched by source title, duration and segment map.
public enum ListingMatch {
    /// Why `new` looks like another disc than `old` (disc.changed.volume / disc.changed.titles), or nil when it may
    /// be the same disc.
    public static func differentDisc(_ old: Listing, _ new: Listing) -> BroMessage? {
        if !old.volumeName.isEmpty && !new.volumeName.isEmpty && old.volumeName != new.volumeName {
            return BroMessage(.discChangedVolume, [("old", .string(old.volumeName)), ("new", .string(new.volumeName))])
        }
        let a = Set(old.titles.map(key)), b = Set(new.titles.map(key))
        if !a.isEmpty && !b.isEmpty && a.isDisjoint(with: b) { return BroMessage(.discChangedTitles) }
        return nil
    }

    /// Maps each of `indices` (titles of `old`) to a title of `new`, preferring the same number. Throws when a title
    /// is missing (disc.titleNotInListing, disc.titleGone), or when `sameTracks` holds it and its track list changed
    /// (disc.tracksChanged).
    public static func mapTitles(_ indices: [Int], from old: Listing, to new: Listing, sameTracks: Set<Int>) throws(BroError) -> [Int: Int] {
        var result: [Int: Int] = [:]
        var used = Set<Int>()
        for i in Set(indices).sorted() {
            guard let t = old.titles.first(where: { $0.index == i }) else {
                throw BroMessage(.discTitleNotInListing, [("title", .integer(Int64(i)))], severity: .error).toError()
            }
            let candidates = new.titles.filter { key($0) == key(t) && !used.contains($0.index) }
            guard let match = candidates.first(where: { $0.index == i }) ?? candidates.first else {
                let source = !t.sourceFile.isEmpty ? t.sourceFile : t.sourceTitleId.map { "#\($0)" } ?? "?"
                throw BroMessage(.discTitleGone, [("title", .integer(Int64(i))), ("duration", .string(t.duration)), ("source", .string(source))],
                                 severity: .error).toError()
            }
            if sameTracks.contains(i) && t.tracks.map(\.kind) != match.tracks.map(\.kind) {
                throw BroMessage(.discTracksChanged, [("title", .integer(Int64(i)))], severity: .error).toError()
            }
            result[i] = match.index
            used.insert(match.index)
        }
        return result
    }

    static func key(_ t: Title) -> String { "\(t.sourceTitleId ?? -1)|\(t.durationSeconds)|\(t.segmentMap)" }
}
