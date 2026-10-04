using System.Collections.Generic;
using System.Linq;
using System.Text;

namespace Bromelia.Domain;

/// <summary>MakeMKV's settings.conf format: <c>key = "value"</c> lines, # comments.</summary>
public static class SettingsConf
{
    /// <summary>Every <c>key = value</c> line (quotes around the value removed); the last of a repeated key wins.</summary>
    public static Dictionary<string, string> Parse(string text)
    {
        var o = new Dictionary<string, string>();
        foreach (var raw in text.Split('\n'))
        {
            var t = raw.Trim();
            if (t.Length == 0 || t[0] == '#') continue;
            int eq = t.IndexOf('=');
            if (eq < 0) continue;
            var key = t.Substring(0, eq).Trim();
            var value = t.Substring(eq + 1).Trim();
            if (value.Length >= 2 && value[0] == '"' && value[^1] == '"') value = value.Substring(1, value.Length - 2);
            if (key.Length > 0) o[key] = value;
        }
        return o;
    }

    /// <summary>A header naming <paramref name="header"/>, then the keys in code point order; a double quote in a
    /// value becomes a single one and a line break a space.</summary>
    public static string Render(IReadOnlyDictionary<string, string> settings, string header)
    {
        var sb = new StringBuilder("#\n# MakeMKV settings file. " + header + ".\n# Changes made here are overwritten before every job.\n#\n\n");
        foreach (var key in settings.Keys.OrderBy(k => k, CodePointOrder.Instance))
            sb.Append(key).Append(" = \"").Append(settings[key].Replace('"', '\'').Replace('\n', ' ')).Append("\"\n");
        return sb.ToString();
    }
}
