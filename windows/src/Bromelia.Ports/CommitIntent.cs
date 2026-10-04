using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>A row of commit_intents with its items: rolled forward at startup (plan §22.3).</summary>
public sealed record CommitIntent(
    Id UnitId,
    Id JobId,
    string Staging,
    string Destination,
    bool Merge,
    bool Quarantine,
    Instant CreatedAt,
    IReadOnlyList<CommitItem> Items);
