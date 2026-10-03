using System.Globalization;
using System.Linq;
using System.Text;

namespace Bromelia.Domain;

/// <summary>Comparing names of shows and movies.</summary>
internal static class Names
{
    /// <summary>Lower case, letters and digits only: "Kauboi-Bibappu!" → "kauboibibappu". Per Unicode scalar,
    /// with simple lower-case mappings, as on the other platforms.</summary>
    public static string Normalize(string s)
    {
        var sb = new StringBuilder();
        foreach (var rune in s.EnumerateRunes())
        {
            var cat = Rune.GetUnicodeCategory(rune);
            if (cat is UnicodeCategory.UppercaseLetter or UnicodeCategory.LowercaseLetter or UnicodeCategory.TitlecaseLetter
                or UnicodeCategory.ModifierLetter or UnicodeCategory.OtherLetter or UnicodeCategory.DecimalDigitNumber)
                sb.Append(Rune.ToLowerInvariant(rune).ToString());
        }
        return sb.ToString();
    }
}
