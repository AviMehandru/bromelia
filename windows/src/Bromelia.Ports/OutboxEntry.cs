using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>A row of outbox: a notification waiting to be sent, and what happened to it.</summary>
public sealed record OutboxEntry(
    Id Id,
    string TargetId,
    Id? JobId,
    BroMessage Title,
    IReadOnlyList<BroMessage> Lines,
    StatusWord Status,
    Instant CreatedAt,
    int Attempts,
    Instant? NextAttemptAt = null,
    Instant? SentAt = null,
    BroError? LastError = null);
