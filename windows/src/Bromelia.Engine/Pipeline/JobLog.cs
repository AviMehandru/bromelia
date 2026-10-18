using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Engine;

/// <summary>A job's log file, &lt;data&gt;/jobs/&lt;id&gt;/log.jsonl (plan §16): one JSON object per line, {t, severity, code,
/// params}, in the canonical encoding written on one line, so every platform writes the same bytes.</summary>
public static class JobLog
{
    public const string FileName = "log.jsonl";

    /// <summary>The lines, each ended by a line break (their seq is left out).</summary>
    public static byte[] Encode(IReadOnlyList<LogLineDto> lines)
    {
        var text = new StringBuilder();
        foreach (var line in lines)
        {
            var members = ((JsonValue.Object)LogLineDto.Json(line)).Members.Where(m => m.Key != "seq").ToList();
            text.Append(OneLine(new JsonValue.Object(members))).Append('\n');
        }
        return Encoding.UTF8.GetBytes(text.ToString());
    }

    /// <summary>The lines of a log file, each with its line number as its seq (from 1). A line that isn't a log line keeps
    /// its number and is left out; so is an unfinished last line (no line break), as the runner counts.</summary>
    public static IReadOnlyList<LogLineDto> Decode(byte[] bytes)
    {
        var lines = new List<LogLineDto>();
        var raw = Encoding.UTF8.GetString(bytes).Split('\n');
        for (int i = 0; i < raw.Length - 1; i++)
        {
            long seq = i + 1;
            var j = JsonValue.Parse(raw[i]);
            if (j?["t"]?.AsString is not { } t || Instant.Parse(t) is not { } at || EnumWire.Parse<Severity>(j["severity"]?.AsString) is not { } severity
                || MessageCode.Parse(j["code"]?.AsString ?? "") is not { } code)
                continue;
            lines.Add(new LogLineDto(seq, at, severity, code, j["params"] as JsonValue.Object ?? new JsonValue.Object(new List<KeyValuePair<string, JsonValue>>())));
        }
        return lines;
    }

    /// <summary>Adds the lines to the end of the job folder's log (the folder made when it isn't there), written
    /// atomically, 0600.</summary>
    public static void Append(IFileSystem fs, string jobDirectory, IReadOnlyList<LogLineDto> lines)
    {
        if (lines.Count == 0) return;
        var path = jobDirectory + "/" + FileName;
        if (!fs.Exists(jobDirectory)) fs.CreateDirectory(jobDirectory, parentsMustExist: false);
        var before = fs.Exists(path) ? fs.Read(path) : Array.Empty<byte>();
        fs.WriteAtomically(path, before.Concat(Encode(lines)).ToArray(), 0x180);
    }

    /// <summary>The canonical encoding on one line: its line breaks and their indentation are formatting only (a
    /// string never holds a raw line break).</summary>
    static string OneLine(JsonValue value)
    {
        var canonical = Encoding.UTF8.GetString(JsonValue.EncodeCanonical(value));
        var sb = new StringBuilder();
        for (int i = 0; i < canonical.Length; i++)
        {
            if (canonical[i] != '\n')
            {
                sb.Append(canonical[i]);
                continue;
            }
            while (i + 1 < canonical.Length && canonical[i + 1] == ' ') i++;
        }
        return sb.ToString();
    }
}
