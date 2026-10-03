using System.Collections.Generic;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>The profile a disc gets: every field set (JSON, config-3.json's ProfileFields), the steps to run (the
/// profile's, then those rules add), and where each part came from.</summary>
public sealed record EffectiveProfile(JsonValue Profile, IReadOnlyList<string> Steps, IReadOnlyList<ResolveTrace> Trace);
