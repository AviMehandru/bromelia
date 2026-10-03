import BroFoundation

/// Checks a ripped MKV against its title in the disc listing.
public enum RipCheck {
    /// Problems: no tracks, no video track when the title has one, no duration, or a duration outside `tolerance`.
    /// Notes: more tracks than listed, or a chapter count other than the listing's (one more is fine: MakeMKV may add
    /// a chapter at 00:00).
    public static func check(_ probe: MkvProbe, title: Title) -> RipCheckResult {
        var problems: [BroMessage] = []
        var notes: [BroMessage] = []
        let types = probe.tracks.map(\.type)
        if types.isEmpty { problems.append(BroMessage(.ripcheckNoTracks)) }
        if !types.isEmpty && !types.contains("video") && title.tracks.contains(where: { $0.kind == .video }) {
            problems.append(BroMessage(.ripcheckNoVideo))
        }
        let expected = Double(title.durationSeconds)
        if expected > 0 {
            if let d = probe.durationSeconds {
                if abs(d - expected) > tolerance(expected).seconds {
                    problems.append(BroMessage(.ripcheckDuration, [("actual", .string(Duration.formatClock(Duration(seconds: d.rounded())))),
                                                                   ("expected", .string(Duration.formatClock(Duration(seconds: expected))))]))
                }
            } else {
                problems.append(BroMessage(.ripcheckNoDuration))
            }
        }
        if !title.tracks.isEmpty && types.count > title.tracks.count {
            notes.append(BroMessage(.ripcheckMoreTracks, [("have", .integer(Int64(types.count))), ("listed", .integer(Int64(title.tracks.count)))]))
        }
        if title.chapters > 1 && (probe.chapterCount < title.chapters || probe.chapterCount > title.chapters + 1) {
            notes.append(BroMessage(.ripcheckChapters, [("have", .integer(Int64(probe.chapterCount))), ("listed", .integer(Int64(title.chapters)))]))
        }
        return RipCheckResult(problems: problems, notes: notes)
    }

    /// The difference allowed between the listed and the actual duration: 5 s or 0.5 %, whichever is larger.
    public static func tolerance(_ expectedSeconds: Double) -> Duration { Duration(seconds: max(5, expectedSeconds * 0.005)) }
}
