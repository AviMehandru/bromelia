using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>A row of replicas: a verified second copy.</summary>
public sealed record ReplicaRecord(
    Id Id,
    Id UnitId,
    string TargetId,
    string Path,
    ReplicaState State,
    Instant? VerifiedAt = null,
    BroMessage? Error = null);
