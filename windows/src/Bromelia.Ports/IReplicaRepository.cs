using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>The replicas table.</summary>
public interface IReplicaRepository
{
    Task Upsert(ReplicaRecord replica);

    Task<IReadOnlyList<ReplicaRecord>> ForUnit(Id unitId);

    /// <summary>Replicas that aren't verified.</summary>
    Task<IReadOnlyList<ReplicaRecord>> Lagging();
}
