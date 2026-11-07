using System.Collections.Generic;
using System.Globalization;
using System.Text.RegularExpressions;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary><c>mkvextract chapters --simple</c> output.</summary>
public static class SimpleChapters
{
    private static readonly Regex ChapterLine = new(@"^CHAPTER\d+=(\d+):(\d+):([\d.]+)", RegexOptions.Multiline | RegexOptions.CultureInvariant);

    /// <summary>The chapter starts, in order: <c>CHAPTER02=00:23:36.815</c> → 1416.815 s. A tool wrote this: a line whose
    /// time can't be read is left out.</summary>
    public static List<Duration> Parse(string text)
    {
        var starts = new List<Duration>();
        foreach (Match m in ChapterLine.Matches(text))
            if (int.TryParse(m.Groups[1].Value, NumberStyles.None, CultureInfo.InvariantCulture, out var hours)
                && int.TryParse(m.Groups[2].Value, NumberStyles.None, CultureInfo.InvariantCulture, out var minutes)
                && double.TryParse(m.Groups[3].Value, NumberStyles.AllowDecimalPoint, CultureInfo.InvariantCulture, out var seconds)
                && double.IsFinite(seconds))
                starts.Add(new Duration(hours * 3600.0 + minutes * 60.0 + seconds));
        return starts;
    }
}
