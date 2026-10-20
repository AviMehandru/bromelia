using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Adapters;

/// <summary>The host's MessageRenderer (plan §16): a message <c>{code, params, cause}</c> as text, from a catalog of ICU
/// MessageFormat texts. The subset shared/messages uses: <c>{x}</c>; plural with =N, one, other and #; select with
/// other; the custom formats bytes, duration and durationPrecise (codes.json's paramTypes). Params of type message,
/// messages and messageList are rendered from the same catalog, and a cause follows as " (cause)". Apostrophes follow
/// ICU: '' is one, and one before { or } (or # in a plural) quotes the text up to the next. It never throws: a missing
/// param shows as {name}, an unknown code as the code, and an unclosed brace as text.</summary>
public sealed class MessageTextRenderer : IMessageRenderer
{
    private static readonly string[] Units = { "B", "KB", "MB", "GB", "TB" };

    private readonly MessageTexts _catalog;

    public MessageTextRenderer(MessageTexts catalog) { _catalog = catalog; }

    public string Render(BroMessage message) => RenderJson(message.ToJson());

    /// <summary>The text of a message in its JSON form <c>{code, params, cause}</c>: a BroError's, or a nested
    /// message's.</summary>
    public string RenderJson(JsonValue message)
    {
        var code = message["code"]?.AsString ?? "";
        var text = _catalog.Text(code);
        var rendered = text is null ? code : Format(text, message["params"], null, code);
        return message["cause"] is { IsNull: false } cause ? rendered + " (" + RenderJson(cause) + ")" : rendered;
    }

    private string Format(string p, JsonValue? args, long? hash, string code)
    {
        var sb = new StringBuilder();
        int i = 0;
        while (i < p.Length)
        {
            char c = p[i];
            if (QuoteStarts(p, i, hash != null)) i = QuoteEnd(p, i, sb);
            else if (c == '#' && hash is { } h) { sb.Append(h.ToString(CultureInfo.InvariantCulture)); i++; }
            else if (c == '{')
            {
                int end = Matching(p, i);
                if (end < 0) { sb.Append(p, i, p.Length - i); break; }
                sb.Append(Argument(p.Substring(i + 1, end - i - 1), args, code));
                i = end + 1;
            }
            else { sb.Append(c); i++; }
        }
        return sb.ToString();
    }

    // An apostrophe that quotes: before another, a brace, or (in a plural's text) a #.
    private static bool QuoteStarts(string p, int i, bool hash) =>
        p[i] == '\'' && i + 1 < p.Length && (p[i + 1] is '\'' or '{' or '}' || (hash && p[i + 1] == '#'));

    // Past the quote at i, appending what it stands for to sb (when given): '' is one apostrophe; otherwise the text up
    // to the next lone apostrophe (or the end), in which '' is one too.
    private static int QuoteEnd(string p, int i, StringBuilder? sb)
    {
        if (p[i + 1] == '\'') { sb?.Append('\''); return i + 2; }
        int j = i + 1;
        while (j < p.Length)
        {
            if (p[j] == '\'')
            {
                if (j + 1 < p.Length && p[j + 1] == '\'') { sb?.Append('\''); j += 2; continue; }
                return j + 1;
            }
            sb?.Append(p[j]);
            j++;
        }
        return j;
    }

    // The brace that closes the one at open; -1 when none does.
    private static int Matching(string p, int open)
    {
        int depth = 0;
        int i = open;
        while (i < p.Length)
        {
            if (QuoteStarts(p, i, false)) { i = QuoteEnd(p, i, null); continue; }
            if (p[i] == '{') depth++;
            else if (p[i] == '}' && --depth == 0) return i;
            i++;
        }
        return -1;
    }

    private string Argument(string body, JsonValue? args, string code)
    {
        var parts = SplitTop(body);
        var name = parts[0].Trim();
        var value = args?[name];
        bool missing = value is null or JsonValue.Null;
        var type = parts.Count > 1 ? parts[1].Trim() : "";
        var style = parts.Count > 2 ? parts[2] : "";
        if (type == "select")
        {
            var branches = Branches(style);
            var key = missing ? null : Plain(value!, "; ");
            var text = key != null && branches.TryGetValue(key, out var t) ? t : branches.GetValueOrDefault("other", "");
            return Format(text, args, null, code);
        }
        if (missing) return "{" + name + "}";
        switch (type)
        {
            case "":
                return Plain(value!, _catalog.IsMessageList(code, name) ? ", " : "; ");
            case "plural":
            {
                long n = value!.AsInteger ?? 0;
                var branches = Branches(style);
                var text = branches.TryGetValue("=" + n.ToString(CultureInfo.InvariantCulture), out var exact) ? exact
                    : n == 1 && branches.TryGetValue("one", out var one) ? one : branches.GetValueOrDefault("other", "");
                return Format(text, args, n, code);
            }
            case "bytes": return Bytes(value!.AsInteger ?? 0);
            case "duration": return Duration(value!.AsNumber ?? 0);
            case "durationPrecise": return DurationPrecise(value!.AsNumber ?? 0);
            default: return Plain(value!, "; ");
        }
    }

    // name [, type [, style]], split at the commas outside braces.
    private static List<string> SplitTop(string s)
    {
        var parts = new List<string>();
        int depth = 0, start = 0, i = 0;
        while (i < s.Length && parts.Count < 2)
        {
            if (s[i] == '{') depth++;
            else if (s[i] == '}') depth--;
            else if (s[i] == ',' && depth == 0) { parts.Add(s.Substring(start, i - start)); start = i + 1; }
            i++;
        }
        parts.Add(s.Substring(start));
        return parts;
    }

    // "key {text} key {text} …"; the first of a key counts, and an unclosed branch ends the list.
    private static Dictionary<string, string> Branches(string s)
    {
        var d = new Dictionary<string, string>(StringComparer.Ordinal);
        int i = 0;
        while (i < s.Length)
        {
            int open = s.IndexOf('{', i);
            if (open < 0) break;
            int close = Matching(s, open);
            if (close < 0) break;
            d.TryAdd(s.Substring(i, open - i).Trim(), s.Substring(open + 1, close - open - 1));
            i = close + 1;
        }
        return d;
    }

    private static bool IsMessage(JsonValue v) => v is JsonValue.Object && v["code"]?.AsString != null;

    private string Plain(JsonValue v, string messageJoin) => v switch
    {
        JsonValue.String s => s.Value,
        JsonValue.Integer i => i.Value.ToString(CultureInfo.InvariantCulture),
        JsonValue.Bool b => b.Value ? "true" : "false",
        JsonValue.Array a when a.Items.Count > 0 && a.Items.All(IsMessage) => string.Join(messageJoin, a.Items.Select(RenderJson)),
        JsonValue.Array a => string.Join(", ", a.Items.Select(x => Plain(x, "; "))),
        JsonValue.Null => "",
        _ when IsMessage(v) => RenderJson(v),
        _ => v.ToString(),
    };

    // Decimal units like a file manager: one decimal, rounded half up, the unit chosen after rounding (999950 is 1.0 MB).
    private static string Bytes(long n)
    {
        if (n < 1000) return n.ToString(CultureInfo.InvariantCulture) + " B";
        int u = 1;
        long tenths;
        while (true)
        {
            long q = 100;
            for (int k = 1; k < u; k++) q *= 1000;
            tenths = n / q + (n % q >= q / 2 ? 1 : 0);
            if (tenths < 10000 || u == Units.Length - 1) break;
            u++;
        }
        return FormattableString.Invariant($"{tenths / 10}.{tenths % 10} {Units[u]}");
    }

    // m:ss, or h:mm:ss from an hour; fractions of a second dropped.
    private static string Duration(double seconds)
    {
        long s = (long)seconds;
        return s >= 3600 ? FormattableString.Invariant($"{s / 3600}:{s / 60 % 60:00}:{s % 60:00}") : FormattableString.Invariant($"{s / 60}:{s % 60:00}");
    }

    // h:mm:ss.sss, rounded to the millisecond.
    private static string DurationPrecise(double seconds)
    {
        long ms = (long)Math.Round(seconds * 1000, MidpointRounding.AwayFromZero);
        return FormattableString.Invariant($"{ms / 3600000}:{ms / 60000 % 60:00}:{ms / 1000 % 60:00}.{ms % 1000:000}");
    }
}
