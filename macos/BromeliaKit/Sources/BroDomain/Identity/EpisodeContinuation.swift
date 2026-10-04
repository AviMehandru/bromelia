import Foundation

/// Where a TV disc's episode numbering continues: after the last episode of the previous disc of its set (same
/// show, season, part and volume; disc number one lower), or, in a media server library, after the highest episode
/// already in the season folder.
public enum EpisodeContinuation {
    /// The previous disc's record when there is one; else the season folder's highest episode, unless a later disc
    /// of the set was archived (the discs were ripped out of order). Nil for the first disc.
    public static func choose(_ query: ContinuationQuery, candidates: [ArchivedDisc]) -> PreviousEpisode? {
        if query.disc <= 1 { return nil }
        let same = candidates.filter { sameSet($0, query: query) }
        if let prev = same.filter({ $0.disc == query.disc - 1 && $0.lastEpisode != nil }).max(by: { $0.lastEpisode! < $1.lastEpisode! }) {
            return PreviousEpisode(lastEpisode: prev.lastEpisode!, source: prev.folder)
        }
        if same.contains(where: { ($0.disc ?? 0) > query.disc }) { return nil }
        guard let last = query.seasonFolderHighest else { return nil }
        return PreviousEpisode(lastEpisode: last, source: query.seasonFolder ?? "")
    }

    static let seasonEpisode = try! NSRegularExpression(pattern: "[Ss]([0-9]{1,3})[Ee]([0-9]{1,4})")

    /// The highest episode number of `season` in file names (`… S02E05 …`), or nil.
    public static func highestInSeason(_ fileNames: [String], season: Int) -> Int? {
        var best: Int?
        for n in fileNames where !n.hasPrefix(".") {
            guard let m = seasonEpisode.firstMatch(in: n, range: NSRange(n.startIndex..., in: n)),
                  let rs = Range(m.range(at: 1), in: n), let re = Range(m.range(at: 2), in: n),
                  Int(n[rs]) == season, let e = Int(n[re]) else { continue }
            if best == nil || e > best! { best = e }
        }
        return best
    }

    /// Whether an archived disc is of the query's set: the same show (its name or its label's title, normalised) and
    /// the same season, part and volume.
    public static func sameSet(_ r: ArchivedDisc, query q: ContinuationQuery) -> Bool {
        let name = Names.normalize(q.name), title = Names.normalize(q.labelTitle)
        let sameShow = (!name.isEmpty && Names.normalize(r.name) == name) || (!title.isEmpty && Names.normalize(r.labelTitle) == title)
        return sameShow && r.season == q.season && r.part == q.part && r.volume == q.volume
    }
}
