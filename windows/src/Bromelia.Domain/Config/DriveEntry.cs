using System.Collections.Generic;
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
    JsonValue? Automation = null);
