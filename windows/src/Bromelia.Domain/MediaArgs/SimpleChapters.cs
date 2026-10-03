using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.RegularExpressions;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary><c>mkvextract chapters --simple</c> output.</summary>
public static class SimpleChapters
{
    private static readonly Regex ChapterLine = new(@"^CHAPTER\d+=(\d+):(\d+):([\d.]+)", RegexOptions.Multiline | RegexOptions.CultureInvariant);

    /// <summary>The chapter starts, in order: <c>CHAPTER02=00:23:36.815</c> → 1416.815 s.</summary>
    public static List<Duration> Parse(string text) =>
        ChapterLine.Matches(text).Select(m => new Duration(int.Parse(m.Groups[1].Value, CultureInfo.InvariantCulture) * 3600
            + int.Parse(m.Groups[2].Value, CultureInfo.InvariantCulture) * 60
            + double.Parse(m.Groups[3].Value, CultureInfo.InvariantCulture))).ToList();
}
