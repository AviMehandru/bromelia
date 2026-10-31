using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Adapters;

/// <summary>The real EngineLog (plan §16; shared/fixtures/adapters/engine-log.cases.json): each line appended to
/// &lt;data&gt;/logs/engine.log through the FileSystem port, the objects a job's log.jsonl holds ({t, severity, code,
/// params}, t this clock's time at the call, params left out when empty) in the canonical encoding on one line. The
/// logs folder is made when it isn't there; the file is 0600. A line that can't be written is dropped. Rotation and the
/// redaction of secret values come with bromeliad (phase 4).</summary>
public sealed class FileEngineLog : IEngineLog
{
    readonly IFileSystem _fs;
    readonly IClock _clock;
    readonly string _path;
    readonly object _lock = new();

    /// <param name="path">&lt;data&gt;/logs/engine.log.</param>
    public FileEngineLog(IFileSystem fs, IClock clock, string path)
    {
        _fs = fs;
        _clock = clock;
        _path = path;
    }

    public void Log(BroMessage message)
    {
        var members = new List<KeyValuePair<string, JsonValue>>
        {
            new("t", JsonValue.Of(Instant.Format(_clock.Now()))),
            new("severity", JsonValue.Of(EnumWire.Name(message.Severity))),
            new("code", JsonValue.Of(MessageCode.Wire(message.Code))),
        };
        if (message.Params.Members.Count > 0) members.Add(new("params", message.Params));
        var line = Encoding.UTF8.GetBytes(OneLine(new JsonValue.Object(members)) + "\n");
        lock (_lock)
        {
            try
            {
                var folder = Path.GetDirectoryName(_path);
                if (!string.IsNullOrEmpty(folder) && !_fs.Exists(folder)) _fs.CreateDirectory(folder, parentsMustExist: false);
                var before = _fs.Exists(_path) ? _fs.Read(_path) : Array.Empty<byte>();
                _fs.WriteAtomically(_path, before.Concat(line).ToArray(), 0x180);
            }
            catch (BroFailure) { }
        }
    }

    /// <summary>The canonical encoding on one line (as JobLog writes): its line breaks and their indentation are
    /// formatting only (a string never holds a raw line break).</summary>
    static string OneLine(JsonValue value)
    {
        var canonical = Encoding.UTF8.GetString(JsonValue.EncodeCanonical(value)).TrimEnd('\n');
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
