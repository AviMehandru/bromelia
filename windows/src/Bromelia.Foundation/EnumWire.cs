using System;

namespace Bromelia.Foundation;

/// <summary>The wire form of an enum case: its name in camel case (<c>SucceededWithReadErrors</c> →
/// <c>succeededWithReadErrors</c>), as in shared/schema/common.json. Swift enums have it as their raw value.</summary>
public static class EnumWire
{
    public static string Name<T>(T value) where T : struct, Enum
    {
        var s = value.ToString();
        return char.ToLowerInvariant(s[0]) + s.Substring(1);
    }

    public static T? Parse<T>(string? text) where T : struct, Enum
    {
        if (string.IsNullOrEmpty(text) || !char.IsLower(text[0])) return null;
        var pascal = char.ToUpperInvariant(text[0]) + text.Substring(1);
        return Enum.TryParse<T>(pascal, false, out var v) && Enum.IsDefined(v) && v.ToString() == pascal ? v : null;
    }
}
