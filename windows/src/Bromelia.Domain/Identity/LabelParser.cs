using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.RegularExpressions;

namespace Bromelia.Domain;

/// <summary>Reads disc labels.</summary>
public static class LabelParser
{
    private static readonly HashSet<string> Noise = new(StringComparer.Ordinal)
    {
        "WS", "FS", "16X9", "4X3", "NTSC", "PAL", "R1", "R2", "R4", "UHD", "4K", "BD", "BLURAY", "BLU", "RAY",
        "DVD", "DVD5", "DVD9", "BD25", "BD50", "BD66", "BD100", "HDR", "SDR", "HD", "DISC", "DISK",
    };
    private static readonly HashSet<string> SmallWords = new(StringComparer.Ordinal)
    {
        "a", "an", "and", "as", "at", "but", "by", "for", "from", "in", "into", "nor", "of", "on", "or", "the", "to", "vs", "with",
    };

    private static readonly Regex SeasonDisc = new(@"^S([0-9]{1,2})(?:D([0-9]{1,2}))?(?:E([0-9]{1,3}))?$", RegexOptions.CultureInvariant);
    private static readonly Regex Season = new(@"^SEASON([0-9]{1,2})?$", RegexOptions.CultureInvariant);
    private static readonly Regex Episode = new(@"^(?:EP|EPS|EPISODE|EPISODES)([0-9]{1,3})?$", RegexOptions.CultureInvariant);
    private static readonly Regex Disc = new(@"^(?:D|DISC|DISK|CD)([0-9]{1,2})$", RegexOptions.CultureInvariant);
    private static readonly Regex Part = new(@"^(?:P|PT|PART)([0-9]{1,2})$", RegexOptions.CultureInvariant);
    private static readonly Regex Volume = new(@"^(?:V|VOL|VOLUME)([0-9]{1,2})$", RegexOptions.CultureInvariant);

    /// <summary>Splits a label into words, reads set markers (S2, SEASON 2, P7, VOL 3, D2, DISC 2, S1D2 …) and
    /// returns the cleaned title. Everything from the first set marker on is left out of the title.</summary>
    public static Label Parse(string label)
    {
        int? season = null, part = null, volume = null, disc = null;
        bool series = false;
        var s = label.Replace("™", "").Replace("®", "").Replace("©", "").Replace(" - ", " ");
        var tokens = s.Split(new[] { '_', '.', ' ', '\t', '(', ')', '[', ']', ',' }, StringSplitOptions.RemoveEmptyEntries)
            .Where(t => t != "-").ToList();
        var titleTokens = new List<string>();
        bool stopped = false;
        int? NumberAfter(int i) => i + 1 < tokens.Count && Digits(tokens[i + 1]) is { } n ? n : null;
        int G(Match m, int g) => int.Parse(m.Groups[g].Value, CultureInfo.InvariantCulture);

        for (int i = 0; i < tokens.Count; i++)
        {
            var t = tokens[i];
            var u = AsciiUpper(t);
            bool marker = true, consumed = false;
            Match m;
            if ((m = SeasonDisc.Match(u)).Success)
            {
                season = G(m, 1);
                if (m.Groups[2].Success) disc = G(m, 2);
                series = true;
            }
            else if ((m = Season.Match(u)).Success)
            {
                if (m.Groups[1].Success) season = G(m, 1);
                else if (NumberAfter(i) is { } n) { season = n; consumed = true; }
                series = true;
            }
            else if ((m = Episode.Match(u)).Success)
            {
                if (!m.Groups[1].Success && NumberAfter(i) != null) consumed = true;
                series = true;
            }
            else if ((m = Disc.Match(u)).Success) disc = G(m, 1);
            else if (u is "DISC" or "DISK" or "D" && NumberAfter(i) is { } dn) { disc = dn; consumed = true; }
            else if ((m = Part.Match(u)).Success) part = G(m, 1);
            else if (u is "PART" or "PT" && NumberAfter(i) is { } pn) { part = pn; consumed = true; }
            else if ((m = Volume.Match(u)).Success) { volume = G(m, 1); series = true; }
            else if (u is "VOL" or "VOLUME" && NumberAfter(i) is { } vn) { volume = vn; consumed = true; series = true; }
            else marker = false;

            if (marker) stopped = true;
            else if (!stopped && !Noise.Contains(u)) titleTokens.Add(t);
            if (consumed) i++;
        }
        return new Label(TitleCase(titleTokens), season, part, volume, disc, series);
    }

    /// <summary>"Season 2 Part 7 Disc 2", or "" when the label has no set information. Part of archive names,
    /// so never translated.</summary>
    public static string SetDescription(Label label)
    {
        var parts = new List<string>();
        if (label.Season is { } s) parts.Add("Season " + s.ToString(CultureInfo.InvariantCulture));
        if (label.Part is { } p) parts.Add("Part " + p.ToString(CultureInfo.InvariantCulture));
        if (label.Volume is { } v) parts.Add("Volume " + v.ToString(CultureInfo.InvariantCulture));
        if (label.Disc is { } d) parts.Add("Disc " + d.ToString(CultureInfo.InvariantCulture));
        return string.Join(" ", parts);
    }

    /// <summary>Title-cases words when the label is all upper or all lower case; mixed case is kept.</summary>
    private static string TitleCase(IReadOnlyList<string> words)
    {
        bool shouting = words.All(w => w == w.ToUpperInvariant()) || words.All(w => w == w.ToLowerInvariant());
        if (!shouting) return string.Join(" ", words);
        var outWords = new List<string>();
        for (int i = 0; i < words.Count; i++)
        {
            var w = words[i];
            var lower = w.ToLowerInvariant();
            if (i > 0 && SmallWords.Contains(lower)) { outWords.Add(lower); continue; }
            if (w.Length <= 4 && lower.All(c => c is 'i' or 'v' or 'x')) { outWords.Add(w.ToUpperInvariant()); continue; }
            outWords.Add(string.Join("-", lower.Split('-').Select(p => p.Length == 0 ? p : p.Substring(0, 1).ToUpperInvariant() + p.Substring(1))));
        }
        return string.Join(" ", outWords);
    }

    private static int? Digits(string s) =>
        s.Length > 0 && s.All(c => c >= '0' && c <= '9') && int.TryParse(s, NumberStyles.None, CultureInfo.InvariantCulture, out var n) ? n : null;

    private static string AsciiUpper(string s)
    {
        var chars = s.ToCharArray();
        for (int i = 0; i < chars.Length; i++)
            if (chars[i] >= 'a' && chars[i] <= 'z') chars[i] = (char)(chars[i] - 32);
        return new string(chars);
    }
}
