using System.Collections.Generic;
using System.Text;

namespace Bromelia.Domain;

/// <summary>Strings in Unicode code point order (UTF-8 byte order), as C's strcmp and LC_ALL=C sort put them;
/// .NET's ordinal order (UTF-16 code units) differs for some characters outside the BMP.</summary>
internal sealed class CodePointOrder : IComparer<string>
{
    public static readonly CodePointOrder Instance = new();

    public int Compare(string? a, string? b)
    {
        var x = Encoding.UTF8.GetBytes(a ?? "");
        var y = Encoding.UTF8.GetBytes(b ?? "");
        for (int i = 0; i < x.Length && i < y.Length; i++)
            if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
        return x.Length.CompareTo(y.Length);
    }
}
