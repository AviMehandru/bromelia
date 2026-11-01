using Bromelia.Foundation;
using Bromelia.Tests;

namespace Bromelia.Scenarios.Tests;

/// <summary>
/// The shared scenarios this runner plays (decisions 233, 234): the JSON copies in shared/scenarios/json, next to
/// shared/fixtures, read as JSON values. A runner selects every scenario with steps whose phase it has built
/// (<see cref="BuiltPhase"/>) and whose platforms, when given, name its own; realDisc scenarios are session I's.
/// </summary>
public static class ScenarioCatalog
{
    /// <summary>The engine phase this runner can run (each phase raises it on all three platforms).</summary>
    public const int BuiltPhase = 3;
    /// <summary>The platform name a scenario's platforms list uses for this runner.</summary>
    public const string Platform = "windows";

    /// <summary>shared/scenarios/json.</summary>
    public static readonly string Folder = Path.GetFullPath(Path.Combine(Fixtures.Root, "..", "scenarios", "json"));

    /// <summary>The ids of the scenarios this runner plays, sorted.</summary>
    public static IReadOnlyList<string> Selected() => Directory.GetFiles(Folder, "*.json")
        .Select(f => Path.GetFileNameWithoutExtension(f)!)
        .Where(id => Selects(Load(id)))
        .OrderBy(id => id, StringComparer.Ordinal)
        .ToList();

    /// <summary>Whether this runner plays the scenario.</summary>
    public static bool Selects(JsonValue scenario) =>
        scenario["steps"] != null && scenario["realDisc"] == null
        && scenario["phase"]?.AsInteger is { } phase && phase <= BuiltPhase
        && (scenario["platforms"]?.AsArray is not { } platforms || platforms.Any(p => p.AsString == Platform));

    /// <summary>A scenario by id.</summary>
    public static JsonValue Load(string id) => Read(id + ".json");

    /// <summary>A file under shared/scenarios/json; a path the scenario gives (tools/x.yaml) is read as its JSON copy.</summary>
    public static JsonValue Read(string relative)
    {
        if (relative.EndsWith(".yaml", StringComparison.Ordinal)) relative = relative[..^5] + ".json";
        var path = Path.Combine(Folder, relative.Replace('/', Path.DirectorySeparatorChar));
        return JsonValue.Parse(File.ReadAllBytes(path)) ?? throw new InvalidOperationException(relative + " isn't JSON");
    }

    /// <summary>A disc: a name in discs/, or a definition written inline.</summary>
    public static JsonValue Disc(JsonValue disc) => disc.AsString is { } name ? Read("discs/" + name + ".json") : disc;

    /// <summary>A tool script: a path (tools/x.yaml), or a script written inline.</summary>
    public static JsonValue Script(JsonValue script) => script.AsString is { } path ? Read(path) : script;

    /// <summary>The named robot lines (tools/lines.yaml).</summary>
    public static IReadOnlyDictionary<string, string> Lines() =>
        Read("tools/lines.json").AsObject!.ToDictionary(m => m.Key, m => m.Value.AsString!);
}
