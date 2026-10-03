using System.Collections.Generic;

namespace Bromelia.Domain;

/// <summary>One layer of an effective profile: which layer, its profile, drive or rule id, and the profile keys it
/// set (dotted paths; the API's ResolveStep).</summary>
public sealed record ResolveTrace(ResolveLayer Layer, string? Id, IReadOnlyList<string> Keys);
