using System.Collections.Generic;
using System.Linq;
using System.Text;

namespace Bromelia.Domain;

/// <summary>Splits a command line like a POSIX shell (quotes and backslash escapes), without expansion. The same
/// on every platform, so configurations stay portable.</summary>
public static class ArgumentSplitter
{
    public static List<string> Split(string text)
    {
        var args = new List<string>();
        var cur = new StringBuilder();
        bool single = false, dbl = false, has = false;
        for (int i = 0; i < text.Length; i++)
        {
            char c = text[i];
            if (single)
            {
                if (c == '\'') single = false; else cur.Append(c);
            }
            else if (dbl)
            {
                if (c == '"') dbl = false;
                else if (c == '\\' && i + 1 < text.Length)
                {
                    char n = text[++i];
                    if (n is '"' or '\\' or '$' or '`') cur.Append(n); else { cur.Append(c); cur.Append(n); }
                }
                else cur.Append(c);
            }
            else if (c == '\'') { single = true; has = true; }
            else if (c == '"') { dbl = true; has = true; }
            else if (c == '\\' && i + 1 < text.Length)
            {
                // Keep Windows paths usable: a backslash only escapes quotes, spaces and backslashes.
                char n = text[i + 1];
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

    /// <summary>An argument quoted for display, POSIX style on every platform: as is when it has only ASCII
    /// letters, digits and <c>-_./:=+,@%</c>, else in single quotes (<c>it's</c> → <c>'it'\''s'</c>).</summary>
    public static string Quote(string argument)
    {
        if (argument.Length > 0 && argument.All(ch => (ch < 0x80 && char.IsLetterOrDigit(ch)) || "-_./:=+,@%".IndexOf(ch) >= 0)) return argument;
        return "'" + argument.Replace("'", "'\\''") + "'";
    }
}
