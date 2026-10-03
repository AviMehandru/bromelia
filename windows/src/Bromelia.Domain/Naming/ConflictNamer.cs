using System.Collections.Generic;
using System.Globalization;
using System.Linq;

namespace Bromelia.Domain;

/// <summary>Names that don't collide.</summary>
public static class ConflictNamer
{
    /// <summary><paramref name="name"/> when no entry of <paramref name="existing"/> has it (ignoring ASCII case,
    /// as some file systems do), else "Name (2)", "Name (3)" … before the extension of a file.</summary>
    public static string Next(string name, IReadOnlyCollection<string> existing, bool isFolder = false)
    {
        var taken = existing.Select(MessageCatalog.AsciiLower).ToHashSet();
        if (!taken.Contains(MessageCatalog.AsciiLower(name))) return name;
        int dot = isFolder ? -1 : name.LastIndexOf('.');
        var stem = dot > 0 ? name.Substring(0, dot) : name;
        var ext = dot > 0 ? name.Substring(dot) : "";
        for (int n = 2; ; n++)
        {
            var candidate = stem + " (" + n.ToString(CultureInfo.InvariantCulture) + ")" + ext;
            if (!taken.Contains(MessageCatalog.AsciiLower(candidate))) return candidate;
        }
    }
}
