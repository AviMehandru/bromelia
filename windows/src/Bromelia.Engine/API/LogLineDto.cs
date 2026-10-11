using System.Collections.Generic;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>A line of a job's log (dtos.json LogLine). jobs/&lt;id&gt;/log.jsonl holds the same objects without
/// their seq.</summary>
public sealed record LogLineDto(long Seq, Instant T, Severity Severity, MessageCode Code, JsonValue.Object Params)
{
    public static JsonValue Json(LogLineDto line)
    {
        var m = new List<KeyValuePair<string, JsonValue>>
        {
            new("seq", JsonValue.Of(line.Seq)),
            new("t", JsonValue.Of(Instant.Format(line.T))),
            new("severity", JsonValue.Of(EnumWire.Name(line.Severity))),
            new("code", JsonValue.Of(MessageCode.Wire(line.Code))),
        };
        if (line.Params.Members.Count > 0) m.Add(new("params", line.Params));
        return new JsonValue.Object(m);
    }
}
