import BroFoundation

/// A profile's naming (config-3.json's `naming` object), with its defaults.
public struct NamingSettings: Sendable, Equatable {
    public static let defaultFolderTemplate = "{name}{discLabel? - {discLabel}}"
    public static let defaultFileNameTemplate =
        "{name}{episode? - {episode}}{episodeTitle? - {episodeTitle}}{discLabel? - {discLabel}} - {rip}{track? - {track}} - {format}"

    public var layout: Layout
    public var folderTemplate: String
    public var fileNameTemplate: String
    public var backupSubfolder: String
    public var conflictPolicy: ConflictPolicy

    public init(layout: Layout = .templates, folderTemplate: String = NamingSettings.defaultFolderTemplate,
                fileNameTemplate: String = NamingSettings.defaultFileNameTemplate, backupSubfolder: String = "backup",
                conflictPolicy: ConflictPolicy = .newFolder) {
        self.layout = layout
        self.folderTemplate = folderTemplate
        self.fileNameTemplate = fileNameTemplate
        self.backupSubfolder = backupSubfolder
        self.conflictPolicy = conflictPolicy
    }

    /// Naming from its JSON (a profile's naming object), with defaults for what it leaves out.
    public static func decode(_ json: JsonValue) -> NamingSettings {
        var issues: [Issue] = []
        let j = SchemaWalker.normalize(json, SchemaWalker.def("ProfileFields")["properties"]!["naming"]!, "", fill: true, &issues)
        return NamingSettings(layout: Layout(rawValue: j["layout"]?.string ?? "") ?? .templates, folderTemplate: j["folderTemplate"]?.string ?? "",
                              fileNameTemplate: j["fileNameTemplate"]?.string ?? "", backupSubfolder: j["backupSubfolder"]?.string ?? "",
                              conflictPolicy: ConflictPolicy(rawValue: j["conflictPolicy"]?.string ?? "") ?? .newFolder)
    }
}
