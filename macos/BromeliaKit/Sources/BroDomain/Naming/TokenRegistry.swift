/// The values of the template tokens.
public enum TokenRegistry {
    /// The tokens shared by every file of a job: name, kind, format, rip, discLabel, discNumber, season, part,
    /// volumeNumber, disc, volume, type, drive, job, date, time, year, month, day, releaseYear, seasonOr1 and
    /// libraryFolder (Movies / TV Shows). The per-file tokens (episode, episodeNumber, episodeTitle, track) are empty
    /// here; each `PlannedOutput` sets its own.
    public static func values(_ identity: Identity, context: TokenContext) -> [String: String] {
        func n(_ v: Int?) -> String { v.map(String.init) ?? "" }
        func pad(_ v: Int, _ w: Int) -> String { let s = String(v); return String(repeating: "0", count: max(0, w - s.count)) + s }
        let t = context.localTime
        return [
            "name": identity.name,
            "kind": identity.kind.rawValue,
            "format": identity.formatCode.text,
            "rip": context.rip,
            "discLabel": LabelParser.setDescription(identity.label),
            "discNumber": n(identity.label.disc),
            "season": n(identity.label.season),
            "part": n(identity.label.part),
            "volumeNumber": n(identity.label.volume),
            "disc": context.disc.isEmpty ? context.volume : context.disc,
            "volume": context.volume,
            "type": context.type.rawValue,
            "drive": context.drive,
            "job": context.job,
            "date": t.map { "\(pad($0.year, 4))-\(pad($0.month, 2))-\(pad($0.day, 2))" } ?? "",
            "time": t.map { "\(pad($0.hour, 2))-\(pad($0.minute, 2))-\(pad($0.second, 2))" } ?? "",
            "year": t.map { pad($0.year, 4) } ?? "",
            "month": t.map { pad($0.month, 2) } ?? "",
            "day": t.map { pad($0.day, 2) } ?? "",
            "releaseYear": n(context.releaseYear),
            "seasonOr1": n(identity.label.season ?? 1),
            "libraryFolder": identity.kind == .tv ? "TV Shows" : "Movies",
            "episode": "",
            "episodeNumber": "",
            "episodeTitle": "",
            "track": "",
        ]
    }

    /// "Title 11" for DVD titles, "Playlist 00800" for Blu-ray playlists, else MakeMKV's title number.
    public static func trackLabel(_ title: Title) -> String {
        let src = title.sourceFile
        if MessageCatalog.asciiLower(src).hasSuffix(".mpls") {
            let file = src.split(whereSeparator: { $0 == "/" || $0 == "\\" }).last.map(String.init) ?? src
            return "Playlist " + file.dropLast(5)
        }
        return "Title \(title.sourceTitleId ?? title.index)"
    }

    /// "Episode 007" for episode 7 at width 3.
    public static func episodeLabel(_ episode: Int, width: Int) -> String {
        let s = String(episode)
        return "Episode " + String(repeating: "0", count: max(0, width - s.count)) + s
    }
}
