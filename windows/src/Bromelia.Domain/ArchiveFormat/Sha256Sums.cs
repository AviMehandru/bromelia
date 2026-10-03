using System;
using System.Collections.Generic;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;

namespace Bromelia.Domain;

/// <summary>SHA256SUMS, in the format <c>sha256sum -c</c> and <c>shasum -a 256 -c</c> read (I4).</summary>
public static class Sha256Sums
{
    public const string FileName = "SHA256SUMS";

    private static readonly Regex Line = new(@"^([0-9a-fA-F]{64}) [ *](.+)$", RegexOptions.CultureInvariant);

    /// <summary>"hash  path" or "hash *path" lines; others are skipped. Hashes come back in lower case.</summary>
    public static List<SumEntry> Parse(string text) =>
        text.Split('\n').Select(l => Line.Match(l.TrimEnd('\r'))).Where(m => m.Success)
            .Select(m => new SumEntry(m.Groups[2].Value, m.Groups[1].Value.ToLowerInvariant())).ToList();

    /// <summary>"&lt;sha256&gt;  &lt;path&gt;" per line, sorted by path (code point order), with a final newline.</summary>
    public static string Render(IEnumerable<SumEntry> entries) =>
        string.Concat(entries.OrderBy(e => e.Path, CodePointOrder.Instance).Select(e => $"{e.Sha256}  {e.Path}\n"));

    /// <summary>The entries of <paramref name="existing"/> (an earlier job's in the same folder) and
    /// <paramref name="entries"/>; a path in both takes the new hash.</summary>
    public static string Merge(string existing, IEnumerable<SumEntry> entries)
    {
        var merged = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var e in Parse(existing)) merged[e.Path] = e.Sha256;
        foreach (var e in entries) merged[e.Path] = e.Sha256;
        return Render(merged.Select(kv => new SumEntry(kv.Key, kv.Value)));
    }

    /// <summary>SHA-256 of bytes in memory, lower-case hex (files are hashed by the adapters, streaming).</summary>
    public static string Hash(byte[] bytes) => Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
}
