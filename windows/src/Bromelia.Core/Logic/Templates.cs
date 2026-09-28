using System.Globalization;
using System.Text;

namespace Bromelia.Core.Logic;

/// <summary>
/// <c>{token}</c> templates for folders, file names and script arguments.
/// <c>{n:3}</c> zero-pads, <c>{token?text}</c> inserts text only when token is non-empty, unknown tokens stay as is.
/// </summary>
public static class TemplateRenderer
{
    public static readonly (string Token, string Help)[] FolderTokens =
    {
        ("name", "Movie or show name (inferred from the disc, or as entered)"),
        ("discLabel", "Place in the set, e.g. Season 2 Part 7 Disc 2 (empty when unknown)"),
        ("format", "Format code: DVD, BR, 4K; DVDe, BRe, 4Ke for backups that are not decrypted"),
        ("rip", "Rip or Backup"), ("kind", "movie or tv"),
        ("season", "Season number from the disc label"), ("discNumber", "Disc number from the disc label"),
        ("part", "Part number from the disc label"), ("volumeNumber", "Volume number from the disc label"),
        ("disc", "Disc name (falls back to the volume label)"), ("volume", "Volume label"),
        ("type", "dvd, bd, hddvd or disc"), ("drive", "Name of the drive configuration"),
        ("date", "Date, yyyy-MM-dd"), ("time", "Time, HH-mm-ss"), ("year", "Year"), ("month", "Month"), ("day", "Day"),
        ("job", "Short job identifier"),
    };

    public static readonly (string Token, string Help)[] FileTokens = FolderTokens.Concat(new[]
    {
        ("episode", "Episode 138 (TV shows; empty for other titles)"), ("episodeNumber", "Episode number alone"),
        ("track", "Source title: Title 11, Title 11 Ch 8-14 (split episodes) or Playlist 00800"),
        ("title", "Title name (or the disc name when the title has none)"), ("index", "MakeMKV title number (0-based)"),
        ("n", "Position of the title in this job (1-based)"), ("source", "Source title ID (playlist / VTS number)"),
        ("duration", "Duration, h-mm-ss"), ("chapters", "Chapter count"),
        ("original", "MakeMKV's original file name without extension"), ("comment", "Title comment reported by MakeMKV"),
    }).ToArray();

    public static readonly (string Token, string Help)[] ScriptTokens = FileTokens.Concat(new[]
    {
        ("outputDir", "Job output folder"), ("file", "Current file (per-file steps) or first file"),
        ("filename", "Current file name (per-file steps)"), ("stem", "Current file name without extension (per-file steps)"),
        ("files", "All produced files (a lone {files} argument expands to one argument per file)"),
        ("status", "success, failed or cancelled"), ("manifest", "Path of the job manifest JSON"), ("device", "OS device of the drive"),
        ("checksums", "Path of the SHA256SUMS file (empty when checksums are off)"),
    }).ToArray();

    public static string Render(string template, IReadOnlyDictionary<string, string> values, Func<string, string>? sanitize = null)
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

    static int MatchingBrace(string s, int open)
    {
        int depth = 0;
        for (int i = open; i < s.Length; i++)
        {
            if (s[i] == '{') depth++;
            if (s[i] == '}' && --depth == 0) return i;
        }
        return -1;
    }

    static string? Expand(string inner, IReadOnlyDictionary<string, string> values, Func<string, string>? sanitize)
    {
        int q = inner.IndexOf('?');
        if (q >= 0)
        {
            var k = inner[..q];
            if (!values.TryGetValue(k, out var cond)) return null;
            return cond.Length == 0 ? "" : Render(inner[(q + 1)..], values, sanitize);
        }
        var key = inner;
        int pad = 0;
        int colon = inner.IndexOf(':');
        if (colon >= 0)
        {
            key = inner[..colon];
            int.TryParse(inner[(colon + 1)..], out pad);
        }
        if (!values.TryGetValue(key, out var v)) return null;
        if (pad > 0 && int.TryParse(v, NumberStyles.None, CultureInfo.InvariantCulture, out var n))
            v = n.ToString(CultureInfo.InvariantCulture).PadLeft(pad, '0');
        return sanitize != null ? sanitize(v) : v;
    }

    /// <summary>Makes a value safe as a single path component on every platform.</summary>
    public static string SanitizeComponent(string s)
    {
        var sb = new StringBuilder(s.Length);
        foreach (var ch in s)
            sb.Append("/\\:*?\"<>|".IndexOf(ch) >= 0 || char.IsControl(ch) ? '-' : ch);
        return sb.ToString().Trim(' ', '.');
    }

    /// <summary>Renders a relative path; values are sanitised and '/' (or '\') in the template separates folders.</summary>
    public static string RenderPath(string template, IReadOnlyDictionary<string, string> values)
    {
        var rendered = Render(template.Replace('\\', '/'), values, SanitizeComponent);
        var parts = rendered.Split('/').Select(SanitizeComponent).Where(p => p.Length > 0 && p != "..");
        return string.Join(Path.DirectorySeparatorChar, parts);
    }

    public static Dictionary<string, string> DateValues(DateTime? when = null)
    {
        var d = when ?? DateTime.Now;
        var ci = CultureInfo.InvariantCulture;
        return new Dictionary<string, string>
        {
            ["date"] = d.ToString("yyyy-MM-dd", ci),
            ["time"] = d.ToString("HH-mm-ss", ci),
            ["year"] = d.ToString("yyyy", ci),
            ["month"] = d.ToString("MM", ci),
            ["day"] = d.ToString("dd", ci),
        };
    }
}

/// <summary>Splits a command line like a POSIX shell (quotes and backslash escapes), without expansion.
/// Used identically on every platform so configurations stay portable.</summary>
public static class ArgumentSplitter
{
    public static List<string> Split(string s)
    {
        var args = new List<string>();
        var cur = new StringBuilder();
        bool single = false, dbl = false, has = false;
        for (int i = 0; i < s.Length; i++)
        {
            char c = s[i];
            if (single)
            {
                if (c == '\'') single = false; else cur.Append(c);
            }
            else if (dbl)
            {
                if (c == '"') dbl = false;
                else if (c == '\\' && i + 1 < s.Length)
                {
                    char n = s[++i];
                    if (n is '"' or '\\' or '$' or '`') cur.Append(n); else { cur.Append(c); cur.Append(n); }
                }
                else cur.Append(c);
            }
            else if (c == '\'') { single = true; has = true; }
            else if (c == '"') { dbl = true; has = true; }
            else if (c == '\\' && i + 1 < s.Length)
            {
                // Keep Windows paths usable: a backslash only escapes quotes, spaces and backslashes.
                char n = s[i + 1];
                if (n is '"' or '\'' or ' ' or '\\') { cur.Append(n); i++; } else cur.Append(c);
                has = true;
            }
            else if (c is ' ' or '\t' or '\n')
            {
                if (has || cur.Length > 0) { args.Add(cur.ToString()); cur.Clear(); has = false; }
            }
            else { cur.Append(c); has = true; }
        }
        if (has || cur.Length > 0) args.Add(cur.ToString());
        return args;
    }

    /// <summary>Quotes an argument for display.</summary>
    public static string Quote(string a)
    {
        if (a.Length > 0 && a.All(ch => char.IsLetterOrDigit(ch) || "-_./:=+,@%\\".IndexOf(ch) >= 0)) return a;
        return "\"" + a.Replace("\"", "\\\"") + "\"";
    }
}
