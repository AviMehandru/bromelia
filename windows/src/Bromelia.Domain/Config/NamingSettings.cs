using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>A profile's naming (config-3.json's <c>naming</c> object), with its defaults.</summary>
public sealed record NamingSettings(
    Layout Layout = Layout.Templates,
    string FolderTemplate = NamingSettings.DefaultFolderTemplate,
    string FileNameTemplate = NamingSettings.DefaultFileNameTemplate,
    string BackupSubfolder = "backup",
    ConflictPolicy ConflictPolicy = ConflictPolicy.NewFolder)
{
    public const string DefaultFolderTemplate = "{name}{discLabel? - {discLabel}}";
    public const string DefaultFileNameTemplate = "{name}{episode? - {episode}}{episodeTitle? - {episodeTitle}}{discLabel? - {discLabel}} - {rip}{track? - {track}} - {format}";

    /// <summary>Naming from its JSON (a profile's naming object), with defaults for what it leaves out.</summary>
    public static NamingSettings Decode(JsonValue json)
    {
        var node = SchemaWalker.Def("ProfileFields")["properties"]!["naming"]!;
        var j = SchemaWalker.Normalize(json, node, "", true, new System.Collections.Generic.List<Issue>());
        return new NamingSettings(EnumWire.Parse<Layout>(j["layout"]?.AsString) ?? Layout.Templates, j["folderTemplate"]!.AsString!,
            j["fileNameTemplate"]!.AsString!, j["backupSubfolder"]!.AsString!,
            EnumWire.Parse<ConflictPolicy>(j["conflictPolicy"]?.AsString) ?? ConflictPolicy.NewFolder);
    }
}
