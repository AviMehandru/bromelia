using System.Linq;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>A post-processing step of the configuration (config-3.json's Step), with its defaults.</summary>
public sealed record StepDefinition(
    string Id,
    StepType Kind,
    string Name = "",
    bool Enabled = true,
    RunOn RunOn = RunOn.Success,
    bool Background = false,
    bool AffectsOutcome = false,
    int TimeoutSeconds = 0,
    CommandSettings? Command = null,
    HandBrakeSettings? Handbrake = null)
{
    /// <summary>A step from its JSON, with the schema's defaults for what it leaves out (background: true for
    /// handbrake, false for command).</summary>
    public static StepDefinition Decode(JsonValue json)
    {
        var issues = new System.Collections.Generic.List<Issue>();
        var j = SchemaWalker.Normalize(json, SchemaWalker.Def("Step"), "", true, issues);
        var kind = EnumWire.Parse<StepType>(j["kind"]?.AsString) ?? StepType.Command;
        var c = SchemaWalker.Normalize(j["command"] ?? JsonValue.Of(), SchemaWalker.Def("Step")["properties"]!["command"]!, "", true, issues);
        var h = SchemaWalker.Normalize(j["handbrake"] ?? JsonValue.Of(), SchemaWalker.Def("Step")["properties"]!["handbrake"]!, "", true, issues);
        return new StepDefinition(
            j["id"]?.AsString ?? "", kind, j["name"]?.AsString ?? "", j["enabled"]?.AsBool ?? true,
            EnumWire.Parse<RunOn>(j["runOn"]?.AsString) ?? RunOn.Success, j["background"]?.AsBool ?? kind == StepType.Handbrake,
            j["affectsOutcome"]?.AsBool ?? false, (int)(j["timeoutSeconds"]?.AsInteger ?? 0),
            new CommandSettings(c["executable"]!.AsString!, c["interpreter"]!.AsString!, c["arguments"]!.AsString!, c["workingDirectory"]!.AsString!,
                c["perFile"]!.AsBool!.Value, c["environment"]!.AsObject!.ToDictionary(m => m.Key, m => m.Value.AsString ?? "")),
            new HandBrakeSettings(h["executable"]!.AsString!, h["preset"]!.AsString!, h["presetFile"]!.AsString!, h["outputPath"]!.AsString!,
                h["extraArguments"]!.AsString!));
    }
}
