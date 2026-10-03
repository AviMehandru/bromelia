using System;
using System.Linq;
using System.Security.Cryptography;
using System.Text;

namespace Bromelia.Domain;

/// <summary>Disc fingerprints, to recognise a disc when it is inserted again.</summary>
public static class Fingerprint
{
    /// <summary>v1, byte for byte as today: <c>v1:</c> and 32 hex digits of SHA-256 over the volume name (else
    /// the name), the title count and, sorted, each title's source title id, length in seconds, segment map and
    /// size in bytes. Null for a listing without titles.</summary>
    public static string? Of(Listing listing)
    {
        if (listing.Titles.Count == 0) return null;
        var volume = listing.VolumeName.Length > 0 ? listing.VolumeName : listing.Name;
        var lines = listing.Titles.Select(t => FormattableString.Invariant($"{t.SourceTitleId ?? -1}|{t.DurationSeconds}|{t.SegmentMap}|{t.SizeBytes}"))
            .OrderBy(l => l, StringComparer.Ordinal);
        var text = new StringBuilder(FormattableString.Invariant($"bromelia-disc-fingerprint 1\nvolume:{volume}\ntitles:{listing.Titles.Count}\n"));
        foreach (var l in lines) text.Append(l).Append('\n');
        var hash = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(text.ToString()))).ToLowerInvariant();
        return "v1:" + hash.Substring(0, 32);
    }
}
