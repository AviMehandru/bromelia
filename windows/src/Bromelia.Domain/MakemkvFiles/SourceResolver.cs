using System;

namespace Bromelia.Domain;

/// <summary>Where a file or folder the user opened leads: MakeMKV opens discs (an image, or a folder holding BDMV,
/// VIDEO_TS or HVDVD_TS), not single files, so a file inside a disc structure opens the disc it belongs to.</summary>
public static class SourceResolver
{
    private static string Parent(string p)
    {
        int i = p.LastIndexOfAny(new[] { '/', '\\' });
        return i < 0 ? "" : i == 0 ? (p.Length > 1 ? p.Substring(0, 1) : "") : p.Substring(0, i);
    }

    private static string Name(string p) => p.Substring(p.LastIndexOfAny(new[] { '/', '\\' }) + 1);

    /// <summary>An image (.iso, .img, .udf), else the folder holding BDMV / VIDEO_TS / HVDVD_TS, looking from the item
    /// itself up to three levels, else the item as a folder.</summary>
    public static MakemkvSource Source(string path, bool isDirectory)
    {
        var p = path.TrimEnd('\\', '/');
        var name = MessageCatalog.AsciiLower(Name(p));
        if (!isDirectory && (name.EndsWith(".iso", StringComparison.Ordinal) || name.EndsWith(".img", StringComparison.Ordinal) || name.EndsWith(".udf", StringComparison.Ordinal)))
            return new MakemkvSource.Iso(p);
        var dir = isDirectory ? p : Parent(p);
        for (int i = 0; i < 4 && dir.Length > 0; i++)
        {
            var upper = Name(dir).ToUpperInvariant();
            if (upper is "BDMV" or "VIDEO_TS" or "HVDVD_TS" && Parent(dir) is { Length: > 0 } root) return new MakemkvSource.File(root);
            dir = Parent(dir);
            if (dir == "/" || dir == "\\") break;
        }
        return new MakemkvSource.File(p);
    }
}
