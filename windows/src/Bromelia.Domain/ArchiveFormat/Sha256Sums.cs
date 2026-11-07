using System;
using System.Collections.Generic;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;

namespace Bromelia.Domain;

/// <summary>SHA256SUMS, in the format <c>sha256sum -c</c> and <c>shasum -a 256 -c</c> read.</summary>
public static class Sha256Sums
{
    public const string FileName = "SHA256SUMS";

    private static readonly Regex Line = new(@"^([0-9a-fA-F]{64}) [ *](.+)$", RegexOptions.CultureInvariant);
    private static readonly UTF8Encoding StrictUtf8 = new(encoderShouldEmitUTF8Identifier: false, throwOnInvalidBytes: true);

    /// <summary>"hash  path" or "hash *path" lines; others are skipped. Hashes come back in lower case.</summary>
    public static List<SumEntry> Parse(string text) =>
        text.Split('\n').Select(l => Line.Match(l.TrimEnd('\r'))).Where(m => m.Success)
            .Select(m => new SumEntry(m.Groups[2].Value, m.Groups[1].Value.ToLowerInvariant())).ToList();

    /// <summary>"&lt;sha256&gt;  &lt;path&gt;" per line, sorted by path (code point order), with a final newline.</summary>
    public static string Render(IEnumerable<SumEntry> entries) =>
        string.Concat(entries.OrderBy(e => e.Path, CodePointOrder.Instance).Select(e => $"{e.Sha256}  {e.Path}\n"));

    /// <summary>The entries of <paramref name="existing"/> (an earlier job's in the same folder) and
    /// <paramref name="entries"/>; a path in both takes the new hash. The file is rewritten from this, so a line that
    /// can't be read follows the entries as it is, byte for byte: it isn't Bromelia's to drop or to repair.</summary>
    public static byte[] Merge(byte[] existing, IEnumerable<SumEntry> entries)
    {
        var merged = new Dictionary<string, string>(StringComparer.Ordinal);
        var unread = new List<byte>();
        for (int start = 0, end; start < existing.Length; start = end + 1)
        {
            end = Array.IndexOf(existing, (byte)'\n', start);
            if (end < 0) end = existing.Length;
            var line = existing[start..(end > start && existing[end - 1] == '\r' ? end - 1 : end)];
            if (line.Length == 0) continue;
            if (EntryOf(line) is { } e) merged[e.Path] = e.Sha256;
            else unread.AddRange(line.Append((byte)'\n'));
        }
        foreach (var e in entries) merged[e.Path] = e.Sha256;
        return Encoding.UTF8.GetBytes(Render(merged.Select(kv => new SumEntry(kv.Key, kv.Value)))).Concat(unread).ToArray();
    }

    static SumEntry? EntryOf(byte[] line)
    {
        try
        {
            return Parse(StrictUtf8.GetString(line)).FirstOrDefault();
        }
        catch (DecoderFallbackException)
        {
            return null;
        }
    }

    /// <summary>SHA-256 of bytes in memory, lower-case hex (a file is hashed as a stream with <see
    /// cref="Sha256Hasher"/>).</summary>
    public static string Hash(byte[] bytes) => Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
}
