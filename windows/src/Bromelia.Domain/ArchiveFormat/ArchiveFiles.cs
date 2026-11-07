using System;
using System.Collections.Generic;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using Bromelia.Foundation;

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

    /// <summary>The unit a job archives: a UUID (version 8, RFC 9562) made from the first 16 bytes of SHA-256 of
    /// "bromelia-unit\n&lt;jobId&gt;", so a rerun of Seal names the same files (bromelia-&lt;unit8&gt;.json) without any
    /// randomness.</summary>
    public static Id UnitId(Id jobId)
    {
        var b = SHA256.HashData(Encoding.UTF8.GetBytes("bromelia-unit\n" + jobId.Value));
        b[6] = (byte)((b[6] & 0x0F) | 0x80);
        b[8] = (byte)((b[8] & 0x3F) | 0x80);
        var hex = Convert.ToHexString(b, 0, 16).ToLowerInvariant();
        return new Id(hex[..8] + "-" + hex[8..12] + "-" + hex[12..16] + "-" + hex[16..20] + "-" + hex[20..32]);
    }

    /// <summary>What a write that was stopped leaves behind: .&lt;name&gt;.bromelia-tmp-&lt;id&gt;
    /// (FileSystem.writeAtomically) and .&lt;name&gt;.part-&lt;id&gt; (DataImager). Startup recovery removes
    /// them.</summary>
    public static bool IsLeftover(string name) =>
        name.StartsWith(".", StringComparison.Ordinal) && (name.Contains(".bromelia-tmp-", StringComparison.Ordinal) || name.Contains(".part-", StringComparison.Ordinal));

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
                if ((f == item || f.StartsWith(item + "/", StringComparison.Ordinal)) && !f.Split('/').Any(c => c.StartsWith('.')))
                    result.Add(f);
        }
        return result.ToList();
    }
}
