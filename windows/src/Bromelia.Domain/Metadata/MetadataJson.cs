using System.Globalization;
using System.Text;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>Helpers shared by the TMDb and OMDb builders and parsers.</summary>
internal static class MetadataJson
{
    /// <summary>RFC 3986 percent-encoding: everything but letters, digits and <c>-._~</c>, as UTF-8.</summary>
    public static string Escape(string s)
    {
        var sb = new StringBuilder();
        foreach (var b in Encoding.UTF8.GetBytes(s))
        {
            char c = (char)b;
            if (c is >= 'A' and <= 'Z' or >= 'a' and <= 'z' or >= '0' and <= '9' or '-' or '.' or '_' or '~') sb.Append(c);
            else sb.Append('%').Append(b.ToString("X2", CultureInfo.InvariantCulture));
        }
        return sb.ToString();
    }

    public static string Num(int n) => n.ToString(CultureInfo.InvariantCulture);

    public static JsonValue? Object(byte[] bytes) => JsonValue.Parse(bytes) is JsonValue.Object o ? o : null;

    public static string? Str(JsonValue? v) => v?.AsString;

    /// <summary>A string other than OMDb's "N/A", else "".</summary>
    public static string Text(JsonValue? v) => Str(v) is { } s && s != "N/A" ? s : "";

    /// <summary>The year at the start of a date or a span ("1994–2004").</summary>
    public static int? Year(JsonValue? v) =>
        Str(v) is { Length: >= 4 } s && int.TryParse(s.Substring(0, 4), NumberStyles.None, CultureInfo.InvariantCulture, out var y) ? y : null;

    public static int? Int(JsonValue? v) => v?.AsInteger is { } n && n >= int.MinValue && n <= int.MaxValue ? (int)n : null;
}
