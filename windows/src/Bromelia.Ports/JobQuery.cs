using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>Which jobs: in a state, of a kind, finished after an instant; newest first, a page at a time.</summary>
public sealed record JobQuery(
    JobState? State,
    JobKind? Kind,
    Instant? FinishedAfter,
    int Limit,
    int Offset);
