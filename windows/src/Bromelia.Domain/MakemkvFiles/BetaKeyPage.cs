using System.Text.RegularExpressions;

namespace Bromelia.Domain;

/// <summary>The forum page where MakeMKV's developer posts the free beta key.</summary>
public static class BetaKeyPage
{
    public const string Url = "https://forum.makemkv.com/forum/viewtopic.php?f=5&t=1053";

    private static readonly Regex InCode = new(@"<code>\s*(T-[A-Za-z0-9@_]{20,})\s*</code>", RegexOptions.CultureInvariant);
    private static readonly Regex Anywhere = new(@"T-[A-Za-z0-9@_]{20,}", RegexOptions.CultureInvariant);

    /// <summary>The key in the post (the T-… string in its code block, else the first one on the page); null when
    /// there is none.</summary>
    public static string? Parse(string html)
    {
        var code = InCode.Match(html);
        if (code.Success) return code.Groups[1].Value;
        var any = Anywhere.Match(html);
        return any.Success ? any.Value : null;
    }
}
