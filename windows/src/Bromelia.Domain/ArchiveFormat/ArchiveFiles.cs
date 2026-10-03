using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.RegularExpressions;

namespace Bromelia.Domain;

/// <summary>Which files of a unit's folder are Bromelia's own, which are media-server metadata, and which files
/// the produced items are.</summary>
public static class ArchiveFiles
{
    private static readonly Regex OwnLog = new(@"^(bromelia|makemkv)(-[0-9a-f]{8})?-(log|debug-log)([- (].*)?\.txt$", RegexOptions.CultureInvariant);

    /// <summary>Files Bromelia writes next to the archived ones that SHA256SUMS doesn't list: SHA256SUMS, the
    /// records (bromelia*.json), the logs (bromelia-log, makemkv-log, makemkv-debug-log; with a unit's short id in
    /// v3: bromelia-1a2b3c4d-log.txt …), and the INCOMPLETE / READ ERRORS notes.</summary>
    public static bool IsOwnFile(string name) =>
        name == Sha256Sums.FileName
        || (name.StartsWith("bromelia", StringComparison.Ordinal) && name.EndsWith(".json", StringComparison.Ordinal))
        || OwnLog.IsMatch(name)
        || name is "INCOMPLETE.txt" or "READ ERRORS.txt";

    /// <summary>Files written for media servers, which SHA256SUMS doesn't list (servers may rewrite them): .nfo
    /// files and poster.jpg.</summary>
    public static bool IsMetadataFile(string name) =>
        name.EndsWith(".nfo", StringComparison.OrdinalIgnoreCase) || name == "poster.jpg";

    /// <summary>The files the produced items are: each produced file, and every file under a produced folder (from
    /// <paramref name="tree"/>, the folder's files), relative paths in code point order, each once; hidden files
    /// (a component starting with a dot) are left out.</summary>
    public static List<string> Expand(IReadOnlyList<string> produced, IReadOnlyList<string> tree)
    {
        var result = new SortedSet<string>(CodePointOrder.Instance);
        foreach (var p in produced)
        {
            var item = p.TrimEnd('/');
            foreach (var f in tree)
                if ((f == item || f.StartsWith(item + "/", StringComparison.Ordinal)) && !f.Split('/').Any(c => c.StartsWith(".")))
                    result.Add(f);
        }
        return result.ToList();
    }
}
