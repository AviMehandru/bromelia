using System.Collections.Generic;
using System.Linq;

namespace Bromelia.Domain;

/// <summary>Where a job's files go: the profile's templates, or the names Plex, Jellyfin and Emby expect.</summary>
public static class Layouts
{
    private const string MediaServerFolder = "{libraryFolder}/{name}{releaseYear? ({releaseYear})}";
    private const string MediaServerMain = "{name}{releaseYear? ({releaseYear})}";
    private const string MediaServerEpisode = "Season {seasonOr1:2}/{name}{releaseYear? ({releaseYear})} - S{seasonOr1:2}E{episodeNumber:2}{episodeTitle? - {episodeTitle}}";
    private const string MediaServerOther = "Other/{name}{releaseYear? ({releaseYear})} - {track}";
    private const string MediaServerBackup = "Backup/{name}{releaseYear? ({releaseYear})} - Backup - {format}";

    /// <summary>The unit's folder, relative to the library: the folder template, or
    /// <c>Movies/Name (Year)</c> / <c>TV Shows/Name (Year)</c> (by <c>kind</c> unless libraryFolder is set).</summary>
    public static string Folder(NamingSettings naming, IReadOnlyDictionary<string, string> values)
    {
        if (naming.Layout != Layout.MediaServer) return TemplateEngine.RenderPath(naming.FolderTemplate, values);
        var v = new Dictionary<string, string>(values);
        if (!v.TryGetValue("libraryFolder", out var lf) || lf.Length == 0)
            v["libraryFolder"] = v.TryGetValue("kind", out var k) && k == "tv" ? "TV Shows" : "Movies";
        return TemplateEngine.RenderPath(MediaServerFolder, v);
    }

    /// <summary>The path of each output, relative to the library: the unit's folder, then the file. Media server:
    /// episodes in <c>Season NN</c>, a movie's main feature by its name, other titles in <c>Other</c>, backups and
    /// images in <c>Backup</c>. Templates: the file name template (MakeMKV's name, <c>original</c>, when it's
    /// empty); a backup goes in the backup subfolder when there is one.</summary>
    public static List<PlannedPath> Paths(NamingSettings naming, IReadOnlyDictionary<string, string> values, IReadOnlyList<PlannedOutput> outputs)
    {
        var folder = Folder(naming, values);
        var result = new List<PlannedPath>();
        foreach (var o in outputs)
        {
            var v = new Dictionary<string, string>(values);
            foreach (var kv in o.Values) v[kv.Key] = kv.Value;
            var file = naming.Layout == Layout.MediaServer ? TemplateEngine.RenderPath(MediaServerTemplate(o, v), v) : TemplatesFile(naming, o, v);
            var path = string.Join("/", new[] { folder, file }.Where(p => p.Length > 0)) + o.Extension;
            result.Add(new PlannedPath(path, o.Role, o.Title, o.Episode));
        }
        return result;
    }

    private static string MediaServerTemplate(PlannedOutput o, IReadOnlyDictionary<string, string> v)
    {
        if (o.Role is PathRole.Backup or PathRole.Image) return MediaServerBackup;
        if (o.Role == PathRole.Episode || (v.TryGetValue("episodeNumber", out var e) && e.Length > 0)) return MediaServerEpisode;
        if (o.MainFeature && v.TryGetValue("kind", out var k) && k == "movie") return MediaServerMain;
        return MediaServerOther;
    }

    private static string TemplatesFile(NamingSettings naming, PlannedOutput o, IReadOnlyDictionary<string, string> v)
    {
        if (o.Role == PathRole.Backup && naming.BackupSubfolder.Length > 0) return TemplateEngine.RenderPath(naming.BackupSubfolder, v);
        if (naming.FileNameTemplate.Length == 0) return Sanitizer.Component(v.TryGetValue("original", out var original) ? original : "");
        return TemplateEngine.RenderPath(naming.FileNameTemplate, v);
    }
}
