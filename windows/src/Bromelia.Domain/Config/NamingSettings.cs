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
}
