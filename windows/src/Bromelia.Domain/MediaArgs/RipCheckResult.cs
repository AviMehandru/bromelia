using System.Collections.Generic;

namespace Bromelia.Domain;

/// <summary>Problems (the file must not be trusted: a wrong or truncated title) and notes (differences worth
/// logging that don't make the file unusable) of RipCheck.check.</summary>
public sealed record RipCheckResult(IReadOnlyList<BroMessage> Problems, IReadOnlyList<BroMessage> Notes);
