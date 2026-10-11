using System.Collections.Generic;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>The jobs of each queue, in order.</summary>
public sealed record QueueOrders(IReadOnlyList<Id> Acquisition, IReadOnlyList<Id> Processing, IReadOnlyList<Id> Maintenance);
