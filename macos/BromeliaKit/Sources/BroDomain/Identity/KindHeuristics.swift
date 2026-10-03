/// Whether a disc is a movie or a TV show.
public enum KindHeuristics {
    /// TV when the label has season / volume markers, the menu plays three or more episodes in one title, or three
    /// or more titles are of episode length; a movie otherwise.
    public static func decide(_ label: Label, listing: Listing?, playAllEpisodes: Int) -> Decision {
        if label.looksLikeSeries { return Decision(value: .tv, reason: BroMessage(.identityReasonLabelMarkers)) }
        if playAllEpisodes >= 3 {
            return Decision(value: .tv, reason: BroMessage(.identityReasonMenuEpisodes, [("count", .integer(Int64(playAllEpisodes)))]))
        }
        if episodeLike(listing?.titles ?? []).count >= 3 { return Decision(value: .tv, reason: BroMessage(.identityReasonEpisodeTitles)) }
        return Decision(value: .movie, reason: BroMessage(.identityReasonNone))
    }

    /// Titles of 10–75 minutes within ±35 % of the median of such titles (none when fewer than two).
    public static func episodeLike(_ titles: [Title]) -> [Title] {
        let candidates = titles.filter { (600...4500).contains($0.durationSeconds) }
        if candidates.count < 2 { return [] }
        let sorted = candidates.map(\.durationSeconds).sorted()
        let median = Double(sorted[sorted.count / 2])
        return candidates.filter { abs(Double($0.durationSeconds) - median) <= median * 0.35 }
    }
}
