using System.Collections.Generic;
using System.Linq;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Engine;

/// <summary>The engine as clients see it (dtos.json SystemStatus): the facts the host gave, when start ran, whether
/// MakeMKV is found and its version, the tools phase 3 runs as the ToolLocator finds them (makemkvcon, mkvmerge), the
/// problems, the libraries, the configuration's version and the last event's seq. MakeMKV's registration is unknown
/// until MakemkvService's phase 5 part.</summary>
public sealed record SystemStatusDto(
    EngineFacts Facts,
    Instant StartedAt,
    bool MakemkvFound,
    string? MakemkvVersion,
    IReadOnlyList<ToolInfo> Tools,
    IReadOnlyList<ProblemDto> Problems,
    IReadOnlyList<LibraryDto> Libraries,
    long ConfigVersion,
    long EventSeq)
{
    /// <summary>Optional members left out when absent.</summary>
    public static JsonValue Json(SystemStatusDto status)
    {
        var makemkv = new List<KeyValuePair<string, JsonValue>> { new("found", JsonValue.Of(status.MakemkvFound)) };
        if (status.MakemkvVersion is { } version) makemkv.Add(new("version", JsonValue.Of(version)));
        makemkv.Add(new("registration", JsonValue.Of("unknown")));
        return JsonValue.Of(
            ("version", JsonValue.Of(status.Facts.Version)),
            ("engine", JsonValue.Of(("pid", JsonValue.Of(status.Facts.Pid)), ("startedAt", JsonValue.Of(Instant.Format(status.StartedAt))),
                ("platform", JsonValue.Of(status.Facts.Platform)), ("dataDir", JsonValue.Of(status.Facts.DataDir)))),
            ("makemkv", new JsonValue.Object(makemkv)),
            ("tools", JsonValue.Of(status.Tools.Select(ToolJson))),
            ("problems", JsonValue.Of(status.Problems.Select(ProblemDto.Json))),
            ("libraries", JsonValue.Of(status.Libraries.Select(LibraryDto.Json))),
            ("configVersion", JsonValue.Of(status.ConfigVersion)),
            ("eventSeq", JsonValue.Of(status.EventSeq)));
    }

    /// <summary>dtos.json ToolInfo: found when it has a path; why (with its severity) when the locator gave one.</summary>
    static JsonValue ToolJson(ToolInfo tool)
    {
        var m = new List<KeyValuePair<string, JsonValue>>
        {
            new("tool", JsonValue.Of(EnumWire.Name(tool.Tool))),
            new("found", JsonValue.Of(tool.Path != null)),
        };
        if (tool.Path is { } path) m.Add(new("path", JsonValue.Of(path)));
        if (tool.Version is { } version) m.Add(new("version", JsonValue.Of(version)));
        if (tool.Why is { } why)
        {
            var reason = new List<KeyValuePair<string, JsonValue>>(((JsonValue.Object)why.ToJson()).Members)
            {
                new("severity", JsonValue.Of(EnumWire.Name(why.Severity))),
            };
            m.Add(new("why", new JsonValue.Object(reason)));
        }
        return new JsonValue.Object(m);
    }
}
