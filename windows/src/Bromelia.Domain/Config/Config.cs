using System.Collections.Generic;
using System.Linq;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>A configuration, version 3 (shared/schema/config-3.json): the document as decoded (keys in schema
/// order, defaults filled outside profiles, unknown keys dropped) and the issues decoding found. Typed views of
/// its entries: <see cref="Profiles"/>, <see cref="Drives"/>, <see cref="Steps"/>, <see cref="Rules"/>.</summary>
public sealed record Config(JsonValue Document, IReadOnlyList<Issue> Issues)
{
    private IReadOnlyList<JsonValue> List(string key) => Document[key]?.AsArray ?? new List<JsonValue>();

    public static List<Profile> Profiles(Config config) => config.List("profiles").Select(p => new Profile(p)).ToList();

    public static List<DriveEntry> Drives(Config config) => config.List("drives").Select(DriveEntry.Decode).ToList();

    public static List<StepDefinition> Steps(Config config) => config.List("steps").Select(StepDefinition.Decode).ToList();

    public static List<Rule> Rules(Config config) => config.List("rules").Select(Rule.Decode).ToList();
}
