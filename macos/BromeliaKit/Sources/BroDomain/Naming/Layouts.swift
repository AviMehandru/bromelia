/// Where a job's files go: the profile's templates, or the names Plex, Jellyfin and Emby expect.
public enum Layouts {
    static let mediaServerFolder = "{libraryFolder}/{name}{releaseYear? ({releaseYear})}"
    static let mediaServerMain = "{name}{releaseYear? ({releaseYear})}"
    static let mediaServerEpisode = "Season {seasonOr1:2}/{name}{releaseYear? ({releaseYear})} - S{seasonOr1:2}E{episodeNumber:2}{episodeTitle? - {episodeTitle}}"
    static let mediaServerOther = "Other/{name}{releaseYear? ({releaseYear})} - {track}"
    static let mediaServerBackup = "Backup/{name}{releaseYear? ({releaseYear})} - Backup - {format}"

    /// The unit's folder, relative to the library: the folder template, or `Movies/Name (Year)` /
    /// `TV Shows/Name (Year)` (by `kind` unless libraryFolder is set).
    public static func folder(_ naming: NamingSettings, values: [String: String]) -> String {
        guard naming.layout == .mediaServer else { return TemplateEngine.renderPath(naming.folderTemplate, values: values) }
        var v = values
        if (v["libraryFolder"] ?? "").isEmpty { v["libraryFolder"] = v["kind"] == "tv" ? "TV Shows" : "Movies" }
        return TemplateEngine.renderPath(mediaServerFolder, values: v)
    }

    /// The path of each output, relative to the library: the unit's folder, then the file. Media server: episodes in
    /// `Season NN`, a movie's main feature by its name, other titles in `Other`, backups and images in `Backup`.
    /// Templates: the file name template (MakeMKV's name, `original`, when it's empty); a backup goes in the backup
    /// subfolder when there is one.
    public static func paths(_ naming: NamingSettings, values: [String: String], outputs: [PlannedOutput]) -> [PlannedPath] {
        let unit = folder(naming, values: values)
        return outputs.map { o in
            let v = values.merging(o.values) { _, new in new }
            let file = naming.layout == .mediaServer ? TemplateEngine.renderPath(mediaServerTemplate(o, v), values: v) : templatesFile(naming, o, v)
            let path = [unit, file].filter { !$0.isEmpty }.joined(separator: "/") + o.extension
            return PlannedPath(path: path, role: o.role, title: o.title, episode: o.episode)
        }
    }

    static func mediaServerTemplate(_ o: PlannedOutput, _ v: [String: String]) -> String {
        if o.role == .backup || o.role == .image { return mediaServerBackup }
        if o.role == .episode || !(v["episodeNumber"] ?? "").isEmpty { return mediaServerEpisode }
        if o.mainFeature && v["kind"] == "movie" { return mediaServerMain }
        return mediaServerOther
    }

    static func templatesFile(_ naming: NamingSettings, _ o: PlannedOutput, _ v: [String: String]) -> String {
        if o.role == .backup && !naming.backupSubfolder.isEmpty { return TemplateEngine.renderPath(naming.backupSubfolder, values: v) }
        if naming.fileNameTemplate.isEmpty { return Sanitizer.component(v["original"] ?? "") }
        return TemplateEngine.renderPath(naming.fileNameTemplate, values: v)
    }
}
