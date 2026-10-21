using System.Collections.Generic;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>An item a job made, by its final name: a file or a folder (a backup) relative to the job's staging folder,
/// which is the unit's folder as it will be in the library; its role in the archive record (title, episode,
/// closingClip, playAll, backup, image, audio, discInfo; null for a file no title owns) and the title it is of.</summary>
public sealed record NamedFile(string Path, string? Role, int? Title)
{
    public static JsonValue Encode(NamedFile file)
    {
        var members = new List<(string, JsonValue)> { ("path", JsonValue.Of(file.Path)) };
        if (file.Role != null) members.Add(("role", JsonValue.Of(file.Role)));
        if (file.Title is { } t) members.Add(("title", JsonValue.Of(t)));
        return JsonValue.Of(members.ToArray());
    }

    public static NamedFile Decode(JsonValue json) =>
        new(json["path"]?.AsString ?? "", json["role"]?.AsString, (int?)json["title"]?.AsInteger);
}
