using System.Collections.Generic;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>What a matching rule does: switch to a profile, merge a profile patch (an RFC 7386 merge patch), add
/// steps after the profile's.</summary>
public sealed record RuleThen(string? Profile, JsonValue? Set, IReadOnlyList<string> Steps);
