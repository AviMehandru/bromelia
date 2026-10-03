/// Ripping chosen titles in one makemkvcon run. `mkv` takes one title or `all`, so a subset can only be ripped in
/// one run when a minimum title length leaves exactly that subset.
public enum OnePass {
    /// Rips with fewer titles gain nothing: the extra listing costs as much as the runs it saves.
    static let minimumTitles = 3

    /// The minimum length that keeps the chosen titles and drops the others, or nil (rip title by title). Listed
    /// lengths are rounded to seconds, so the longest title left out must be at least 2 s shorter than the shortest
    /// chosen; the length must also be longer than the one already in use.
    public static func plan(_ indices: [Int], listing: Listing, currentMinLength: Int?) -> OnePassPlan? {
        let set = Set(indices)
        if set.count < minimumTitles || set.count >= listing.titles.count { return nil }
        let picked = listing.titles.filter { set.contains($0.index) }.map(\.durationSeconds)
        let others = listing.titles.filter { !set.contains($0.index) }.map(\.durationSeconds)
        guard picked.count == set.count, let shortest = picked.min(), let longestOther = others.max(), shortest - longestOther >= 2 else { return nil }
        let length = longestOther + 1
        return length > (currentMinLength ?? 0) ? OnePassPlan(minLength: length) : nil
    }

    /// Whether `listing` (read with that minimum length) holds exactly the chosen titles of `original`.
    public static func matches(_ listing: Listing, chosen: [Int], of original: Listing) -> Bool {
        let set = Set(chosen)
        if listing.titles.count != set.count { return false }
        guard let map = try? ListingMatch.mapTitles(Array(set), from: original, to: listing, sameTracks: []) else { return false }
        return Set(map.values) == Set(listing.titles.map(\.index))
    }
}
