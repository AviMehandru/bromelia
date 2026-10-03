using System;
using System.Globalization;

namespace Bromelia.Foundation;

/// <summary>A length of time in seconds.</summary>
public readonly record struct Duration(double Seconds)
{
    /// <summary>makemkvcon's <c>h:mm:ss</c> (or <c>m:ss</c>, or seconds): <c>2:44:48</c> → 9888 s. A part that
    /// isn't a number counts as 0, as today; null when the text is empty.</summary>
    public static Duration? ParseClock(string text)
    {
        if (string.IsNullOrWhiteSpace(text)) return null;
        long total = 0;
        foreach (var part in text.Split(':'))
            total = total * 60 + (long.TryParse(part.Trim(), NumberStyles.None, CultureInfo.InvariantCulture, out var n) ? n : 0);
        return new Duration(total);
    }

    /// <summary><c>h:mm:ss</c>, whole seconds: 9888 s → <c>2:44:48</c>, 59 s → <c>0:00:59</c>.</summary>
    public static string FormatClock(Duration duration)
    {
        var s = (long)Math.Floor(Math.Max(0, duration.Seconds));
        return string.Format(CultureInfo.InvariantCulture, "{0}:{1:00}:{2:00}", s / 3600, s / 60 % 60, s % 60);
    }
}
