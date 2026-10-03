using System.Text;

namespace Bromelia.Domain;

/// <summary>Safe file names.</summary>
public static class Sanitizer
{
    /// <summary>A value made safe as one path component on every platform: / \ : * ? " &lt; &gt; | and control
    /// characters become <c>-</c>; spaces and dots are trimmed from both ends.</summary>
    public static string Component(string text)
    {
        var sb = new StringBuilder(text.Length);
        foreach (var ch in text)
            sb.Append("/\\:*?\"<>|".IndexOf(ch) >= 0 || ch < 0x20 || (ch >= 0x7F && ch <= 0x9F) ? '-' : ch);
        return sb.ToString().Trim(' ', '.');
    }
}
