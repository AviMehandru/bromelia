using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>Repositories over shared/schema/db; every write goes through one writer.</summary>
public interface IStore
{
    IJobRepository Jobs();

    IStepRepository Steps();

    IUnitRepository Units();

    ICatalogRepository Catalog();

    ICheckRepository Checks();

    IReplicaRepository Replicas();

    IDriveRepository Drives();

    ILookupCacheRepository LookupCache();

    IOutboxRepository Outbox();

    IKeyValueRepository Kv();

    /// <summary>Runs block in one transaction; a failure rolls it back.</summary>
    Task<T> Transaction<T>(Func<IStore, Task<T>> block);

    /// <summary>Applies the migrations the database lacks (shared/schema/db).</summary>
    Task Migrate();
}
