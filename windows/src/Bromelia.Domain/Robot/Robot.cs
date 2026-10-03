using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;

namespace Bromelia.Domain;

/// <summary>makemkvcon's robot mode (<c>-r</c>): lines → events.</summary>
public static class Robot
{
    /// <summary>Splits comma-separated fields; a quoted field may contain commas and <c>\"</c> / <c>\\</c>
    /// escapes.</summary>
    public static List<string> SplitFields(string body)
    {
        var fields = new List<string>();
        var cur = new StringBuilder();
        bool inQuotes = false, wasQuoted = false;
        for (int i = 0; i < body.Length; i++)
        {
            char c = body[i];
            if (inQuotes)
            {
                if (c == '\\' && i + 1 < body.Length)
                {
                    char n = body[i + 1];
                    if (n == '"' || n == '\\') { cur.Append(n); i++; }
                    else cur.Append(c);
                }
                else if (c == '"') inQuotes = false;
                else cur.Append(c);
            }
            else if (c == ',')
            {
                fields.Add(cur.ToString());
                cur.Clear();
                wasQuoted = false;
            }
            else if (c == '"' && cur.Length == 0 && !wasQuoted)
            {
                inQuotes = true;
                wasQuoted = true;
            }
            else cur.Append(c);
        }
        fields.Add(cur.ToString());
        return fields;
    }

    /// <summary>One line of output. Null for an empty line; <see cref="RobotEvent.Raw"/> for a line that isn't
    /// a robot record (or is one with fields missing).</summary>
    public static RobotEvent? ParseLine(string line)
    {
        line = line.TrimEnd('\r', '\n');
        if (line.Length == 0) return null;
        int colon = line.IndexOf(':');
        if (colon < 0) return new RobotEvent.Raw(line);
        var tag = line.Substring(0, colon);
        var body = line.Substring(colon + 1);
        List<string> f;
        switch (tag)
        {
            case "MSG":
                f = SplitFields(body);
                if (f.Count >= 5 && Int(f[0], out var code) && Int(f[1], out var flags))
                    return new RobotEvent.Message(new RobotMessage(code, flags, Int(f[2], out var count) ? count : 0, f[3], f[4], f.Skip(5).ToList()));
                break;
            case "PRGC":
            case "PRGT":
                f = SplitFields(body);
                if (f.Count >= 3 && Int(f[0], out var pc) && Int(f[1], out var pid))
                    return tag == "PRGC" ? new RobotEvent.ProgressCurrent(pc, pid, f[2]) : new RobotEvent.ProgressTotal(pc, pid, f[2]);
                break;
            case "PRGV":
                f = SplitFields(body);
                if (f.Count >= 3 && Int(f[0], out var a) && Int(f[1], out var b) && Int(f[2], out var m))
                    return new RobotEvent.ProgressValue(a, b, m);
                break;
            case "DRV":
                f = SplitFields(body);
                if (f.Count >= 7 && Int(f[0], out var idx) && Int(f[1], out var st) && Int(f[3], out var df))
                    return new RobotEvent.Drive(idx, st, df, f[4], f[5], f[6]);
                break;
            case "TCOUNT":
                if (Int(body, out var n)) return new RobotEvent.TitleCount(n);
                break;
            case "CINFO":
                f = SplitFields(body);
                if (f.Count >= 3 && Int(f[0], out var cid) && Int(f[1], out var ccode))
                    return new RobotEvent.DiscInfo(cid, ccode, f[2]);
                break;
            case "TINFO":
                f = SplitFields(body);
                if (f.Count >= 4 && Int(f[0], out var tt) && Int(f[1], out var tid) && Int(f[2], out var tcode))
                    return new RobotEvent.TitleInfo(tt, tid, tcode, f[3]);
                break;
            case "SINFO":
                f = SplitFields(body);
                if (f.Count >= 5 && Int(f[0], out var sti) && Int(f[1], out var ss) && Int(f[2], out var sid) && Int(f[3], out var scode))
                    return new RobotEvent.StreamInfo(sti, ss, sid, scode, f[4]);
                break;
        }
        return new RobotEvent.Raw(line);
    }

    /// <summary>An integer field: optional sign and ASCII digits, surrounding spaces allowed.</summary>
    internal static bool Int(string s, out int v) =>
        int.TryParse(s.Trim(' ', '\t'), NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out v);
}
