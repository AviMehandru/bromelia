using System.Collections.Generic;

namespace Bromelia.Domain;

/// <summary>The titles the rules chose (in index order), the reason for every title, whether the user must
/// choose, and the error that stopped the rules (an invalid pattern).</summary>
public sealed record Selection(IReadOnlyList<int> Indices, IReadOnlyList<SelectionTrace> Trace, bool RequiresManualChoice, BroMessage? Error);
