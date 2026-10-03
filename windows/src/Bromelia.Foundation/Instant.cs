using System.Globalization;

namespace Bromelia.Foundation;

/// <summary>A UTC time with milliseconds, held as milliseconds since 1970-01-01T00:00:00Z.</summary>
public readonly record struct Instant(long UnixMilliseconds)
{
    /// <summary>RFC 3339: <c>YYYY-MM-DDTHH:MM:SS</c>, optional fractional seconds (cut to milliseconds), then
    /// <c>Z</c> or <c>±HH:MM</c>. Null for anything else. Written by hand, as on the other platforms, so all
    /// three accept exactly the same texts.</summary>
    public static Instant? Parse(string text)
    {
        int i = 0;
        int Number(int digits)
        {
            if (i + digits > text.Length) return -1;
            int v = 0;
            for (int k = 0; k < digits; k++)
            {
                char c = text[i + k];
                if (c < '0' || c > '9') return -1;
                v = v * 10 + (c - '0');
            }
            i += digits;
            return v;
        }
        bool Literal(char c)
        {
            if (i >= text.Length || text[i] != c) return false;
            i++;
            return true;
        }

        int year = Number(4);
        if (year < 0 || !Literal('-')) return null;
        int month = Number(2);
        if (month < 1 || month > 12 || !Literal('-')) return null;
        int day = Number(2);
        if (day < 1 || day > DaysInMonth(year, month) || !Literal('T')) return null;
        int hour = Number(2);
        if (hour < 0 || hour > 23 || !Literal(':')) return null;
        int minute = Number(2);
        if (minute < 0 || minute > 59 || !Literal(':')) return null;
        int second = Number(2);
        if (second < 0 || second > 59) return null;
        int millis = 0;
        if (Literal('.'))
        {
            int digits = 0;
            while (i < text.Length && text[i] >= '0' && text[i] <= '9')
            {
                if (digits < 3) millis = millis * 10 + (text[i] - '0');
                digits++;
                i++;
            }
            if (digits == 0) return null;
            for (int k = digits; k < 3; k++) millis *= 10;
        }
        int offsetMinutes = 0;
        if (!Literal('Z'))
        {
            int sign = Literal('+') ? 1 : Literal('-') ? -1 : 0;
            if (sign == 0) return null;
            int oh = Number(2);
            if (oh < 0 || oh > 23 || !Literal(':')) return null;
            int om = Number(2);
            if (om < 0 || om > 59) return null;
            offsetMinutes = sign * (oh * 60 + om);
        }
        if (i != text.Length) return null;
        long days = DaysFromCivil(year, month, day);
        long seconds = days * 86400 + hour * 3600 + minute * 60 + second - offsetMinutes * 60L;
        return new Instant(seconds * 1000 + millis);
    }

    /// <summary><c>2026-10-03T10:10:28.608Z</c>.</summary>
    public static string Format(Instant instant)
    {
        long ms = instant.UnixMilliseconds;
        long days = FloorDiv(ms, 86_400_000);
        long inDay = ms - days * 86_400_000;
        var (y, m, d) = CivilFromDays(days);
        return string.Format(CultureInfo.InvariantCulture, "{0:0000}-{1:00}-{2:00}T{3:00}:{4:00}:{5:00}.{6:000}Z",
            y, m, d, inDay / 3_600_000, inDay / 60_000 % 60, inDay / 1000 % 60, inDay % 1000);
    }

    public override string ToString() => Format(this);

    private static long FloorDiv(long a, long b) => a / b - (a % b != 0 && (a < 0) != (b < 0) ? 1 : 0);

    private static int DaysInMonth(int y, int m) =>
        m == 2 ? (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0) ? 29 : 28) : m is 4 or 6 or 9 or 11 ? 30 : 31;

    // Howard Hinnant's days_from_civil / civil_from_days.
    private static long DaysFromCivil(long y, long m, long d)
    {
        y -= m <= 2 ? 1 : 0;
        long era = FloorDiv(y, 400);
        long yoe = y - era * 400;
        long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + doe - 719468;
    }

    private static (long Year, long Month, long Day) CivilFromDays(long z)
    {
        z += 719468;
        long era = FloorDiv(z, 146097);
        long doe = z - era * 146097;
        long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        long mp = (5 * doy + 2) / 153;
        long d = doy - (153 * mp + 2) / 5 + 1;
        long m = mp + (mp < 10 ? 3 : -9);
        return (yoe + era * 400 + (m <= 2 ? 1 : 0), m, d);
    }
}
