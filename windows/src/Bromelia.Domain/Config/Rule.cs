using System.Collections.Generic;
using System.Linq;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>A rule of the configuration (config-3.json's Rule): when → then. Replaces plugins, formatModes and
/// matchName / matchFormats.</summary>
public sealed record Rule(string Id, string Name, bool Enabled, RuleWhen When, RuleThen Then)
{
    public static Rule Decode(JsonValue json)
    {
        List<string>? Strings(JsonValue? v) => v?.AsArray?.Select(x => x.AsString ?? "").ToList();
        var w = json["when"] ?? JsonValue.Of();
        var t = json["then"] ?? JsonValue.Of();
        return new Rule(json["id"]?.AsString ?? "", json["name"]?.AsString ?? "", json["enabled"]?.AsBool ?? true,
            new RuleWhen(w["nameOrLabel"]?.AsString, w["name"]?.AsString, w["label"]?.AsString, Strings(w["formats"]), Strings(w["kinds"]),
                Strings(w["drives"]), Strings(w["profiles"]), w["automatic"]?.AsBool),
            new RuleThen(t["profile"]?.AsString, t["set"], Strings(t["steps"]) ?? new List<string>()));
    }
}
