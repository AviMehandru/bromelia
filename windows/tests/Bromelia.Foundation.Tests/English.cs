using System.Globalization;
using System.Text;
using Bromelia.Foundation;

namespace Bromelia.Tests;

/// <summary>Renders a message <c>{code, params}</c> in English from shared/messages/en.json, for fixtures that
/// check a message's text. The subset of ICU MessageFormat en.json uses: <c>{x}</c>, plural (with =N, one, other
/// and #), select, bytes, duration, durationPrecise, and nested messages (params of type message / messages).
/// Apostrophes are plain text. (The engine host's MessageRenderer, in Adapters, will do this properly.)</summary>
public static class English
{
    private static readonly JsonValue Texts = Fixtures.SharedJson("messages/en.json");
    private static readonly JsonValue Codes = Fixtures.SharedJson("messages/codes.json")["codes"]!;

    public static string Render(JsonValue message)
    {
        var code = message["code"]?.AsString ?? "";
        var text = Texts[code]?.AsString ?? code;
        var rendered = Format(text, message["params"] ?? JsonValue.Of(), null, code);
        return message["cause"] is { } cause && !cause.IsNull ? rendered + " (" + Render(cause) + ")" : rendered;
    }

    private static string Format(string pattern, JsonValue args, long? hash, string code)
    {
        var sb = new StringBuilder();
        int i = 0;
        while (i < pattern.Length)
        {
            char c = pattern[i];
            if (c == '#' && hash is { } h) { sb.Append(h.ToString(CultureInfo.InvariantCulture)); i++; continue; }
            if (c != '{') { sb.Append(c); i++; continue; }
            int end = Matching(pattern, i);
            sb.Append(Argument(pattern.Substring(i + 1, end - i - 1), args, code));
            i = end + 1;
        }
        return sb.ToString();
    }

    private static int Matching(string s, int open)
    {
        int depth = 0;
        for (int i = open; i < s.Length; i++)
        {
            if (s[i] == '{') depth++;
            else if (s[i] == '}' && --depth == 0) return i;
        }
        throw new FormatException("unbalanced braces in " + s);
    }

    private static string Argument(string body, JsonValue args, string code)
    {
        var parts = SplitTop(body, 3);
        var name = parts[0].Trim();
        var value = args[name];
        // messages are joined with "; ", messageList with ", " (codes.json's paramTypes).
        if (parts.Count == 1) return Plain(value, Codes[code]?["params"]?[name]?.AsString == "messageList" ? ", " : "; ");
        var type = parts[1].Trim();
        switch (type)
        {
            case "plural":
            {
                long n = value?.AsInteger ?? 0;
                var branches = Branches(parts[2]);
                var text = branches.TryGetValue("=" + n.ToString(CultureInfo.InvariantCulture), out var exact) ? exact
                    : n == 1 && branches.TryGetValue("one", out var one) ? one : branches["other"];
                return Format(text, args, n, code);
            }
            case "select":
            {
                var key = value is JsonValue.Bool b ? (b.Value ? "true" : "false") : Plain(value);
                var branches = Branches(parts[2]);
                return Format(branches.TryGetValue(key, out var t) ? t : branches["other"], args, null, code);
            }
            case "bytes": return Bytes(value?.AsInteger ?? 0);
            case "duration": return Clock((long)(value?.AsNumber ?? 0), false);
            case "durationPrecise": return Clock((long)(value?.AsNumber ?? 0), true);
            default: return Plain(value);
        }
    }

    private static string Plain(JsonValue? v, string messageJoin = "; ") => v switch
    {
        null or JsonValue.Null => "",
        JsonValue.String s => s.Value,
        JsonValue.Integer i => i.Value.ToString(CultureInfo.InvariantCulture),
        JsonValue.Bool b => b.Value ? "true" : "false",
        JsonValue.Array a when a.Items.All(x => x["code"] != null) => string.Join(messageJoin, a.Items.Select(Render)),
        JsonValue.Array a => string.Join(", ", a.Items.Select(x => Plain(x))),
        JsonValue.Object o when o["code"] != null => Render(o),
        _ => v.ToString().Trim(),
    };

    private static List<string> SplitTop(string s, int max)
    {
        var parts = new List<string>();
        int depth = 0, start = 0;
        for (int i = 0; i < s.Length && parts.Count < max - 1; i++)
        {
            if (s[i] == '{') depth++;
            else if (s[i] == '}') depth--;
            else if (s[i] == ',' && depth == 0) { parts.Add(s.Substring(start, i - start)); start = i + 1; }
        }
        parts.Add(s.Substring(start));
        return parts;
    }

    private static Dictionary<string, string> Branches(string s)
    {
        var d = new Dictionary<string, string>();
        int i = 0;
        while (i < s.Length)
        {
            while (i < s.Length && char.IsWhiteSpace(s[i])) i++;
            if (i >= s.Length) break;
            int open = s.IndexOf('{', i);
            var key = s.Substring(i, open - i).Trim();
            int close = Matching(s, open);
            d[key] = s.Substring(open + 1, close - open - 1);
            i = close + 1;
        }
        return d;
    }

    private static string Bytes(long n)
    {
        string[] units = { "B", "KB", "MB", "GB", "TB" };
        double v = n;
        int u = 0;
        while (v >= 1000 && u < units.Length - 1) { v /= 1000; u++; }
        return u == 0 ? $"{n} B" : v.ToString("0.0", CultureInfo.InvariantCulture) + " " + units[u];
    }

    private static string Clock(long s, bool precise) =>
        s >= 3600 ? $"{s / 3600}:{s / 60 % 60:00}:{s % 60:00}" : $"{s / 60}:{s % 60:00}";
}
