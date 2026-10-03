using System.Collections.Generic;
using System.Linq;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>A drive of the configuration (config-3.json's <c>Drive</c>): its profile (null = the default
/// profile), MakeMKV settings of its own and automation (null = automation.defaults).</summary>
public sealed record DriveEntry(
    string Id,
    DriveMatch Match,
    string Name = "Drive",
    bool Enabled = true,
    string? Profile = null,
    IReadOnlyDictionary<string, string>? MakemkvSettings = null,
    JsonValue? Automation = null)
{
    /// <summary>A drive entry from its JSON, with the schema's defaults for what it leaves out.</summary>
    public static DriveEntry Decode(JsonValue json)
    {
        var m = json["match"];
        return new DriveEntry(json["id"]?.AsString ?? "", new DriveMatch(m?["driveName"]?.AsString ?? "", m?["devicePath"]?.AsString ?? ""),
            json["name"]?.AsString ?? "Drive", json["enabled"]?.AsBool ?? true, json["profile"]?.AsString,
            (json["makemkvSettings"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>()).ToDictionary(x => x.Key, x => x.Value.AsString ?? ""),
            json["automation"] is { IsNull: false } a ? a : null);
    }
}
