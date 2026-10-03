using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;

namespace Bromelia.Domain;

/// <summary><c>{token}</c> templates for folders, file names and script arguments. <c>{n:3}</c> zero-pads,
/// <c>{token?text}</c> inserts text only when the token is set and not empty, other unknown tokens stay as they
/// are.</summary>
public static class TemplateEngine
{
    public static string Render(string template, IReadOnlyDictionary<string, string> values) => Render(template, values, null);

    /// <summary>A relative path: values are made safe, <c>/</c> (or <c>\</c>) in the template separates folders,
    /// every component is made safe, and empty, <c>.</c> and <c>..</c> components are dropped. Folders are
    /// separated by <c>/</c> on every platform.</summary>
    public static string RenderPath(string template, IReadOnlyDictionary<string, string> values)
    {
        var rendered = Render(template.Replace('\\', '/'), values, Sanitizer.Component);
        return string.Join("/", rendered.Split('/').Select(Sanitizer.Component).Where(p => p.Length > 0 && p != ".."));
    }

    private static string Render(string template, IReadOnlyDictionary<string, string> values, Func<string, string>? sanitize)
    {
        var sb = new StringBuilder();
        int i = 0;
        while (i < template.Length)
        {
            char c = template[i];
            if (c == '{')
            {
                int close = MatchingBrace(template, i);
                if (close > i)
                {
                    var inner = template.Substring(i + 1, close - i - 1);
                    sb.Append(Expand(inner, values, sanitize) ?? "{" + inner + "}");
                    i = close + 1;
                    continue;
                }
            }
            sb.Append(c);
            i++;
        }
        return sb.ToString();
    }

    private static int MatchingBrace(string s, int open)
    {
        int depth = 0;
        for (int i = open; i < s.Length; i++)
        {
            if (s[i] == '{') depth++;
            if (s[i] == '}' && --depth == 0) return i;
        }
        return -1;
    }

    private static string? Expand(string inner, IReadOnlyDictionary<string, string> values, Func<string, string>? sanitize)
    {
        int q = inner.IndexOf('?');
        if (q >= 0)
        {
            // A condition on a token that isn't set is false.
            return values.TryGetValue(inner.Substring(0, q), out var cond) && cond.Length > 0 ? Render(inner.Substring(q + 1), values, sanitize) : "";
        }
        var key = inner;
        int pad = 0;
        int colon = inner.IndexOf(':');
        if (colon >= 0)
        {
            key = inner.Substring(0, colon);
            if (!Robot.Int(inner.Substring(colon + 1), out pad)) pad = 0;
        }
        if (!values.TryGetValue(key, out var v)) return null;
        if (pad > 0 && v.Length > 0 && v.All(ch => ch >= '0' && ch <= '9') && int.TryParse(v, NumberStyles.None, CultureInfo.InvariantCulture, out var n))
            v = n.ToString(CultureInfo.InvariantCulture).PadLeft(pad, '0');
        return sanitize != null ? sanitize(v) : v;
    }
}
