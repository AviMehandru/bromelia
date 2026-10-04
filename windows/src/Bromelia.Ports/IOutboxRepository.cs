using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>The outbox table.</summary>
public interface IOutboxRepository
{
    Task Add(OutboxEntry notification);

    /// <summary>Unsent notifications whose next attempt is due.</summary>
    Task<IReadOnlyList<OutboxEntry>> Due(Instant now);

    Task MarkSent(Id id);

    /// <summary>No retryAt: give up.</summary>
    Task MarkFailed(Id id, BroError error, Instant? retryAt);
}
