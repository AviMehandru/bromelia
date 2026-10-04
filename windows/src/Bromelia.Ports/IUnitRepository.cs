using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>archive_units, unit_files and the commit intents.</summary>
public interface IUnitRepository
{
    /// <summary>The unit (committing) and its intent, in one transaction.</summary>
    Task BeginCommit(UnitRecord unit, CommitIntent intent);

    /// <summary>Item seq of the intent has been moved.</summary>
    Task MarkMoved(Id unitId, int seq);

    /// <summary>The committed unit and its files; the intent goes.</summary>
    Task FinishCommit(CommitIntent intent, UnitRecord record, IReadOnlyList<UnitFile> files);

    Task<IReadOnlyList<CommitIntent>> OpenIntents();

    Task<UnitRecord?> Get(Id unitId);

    Task<IReadOnlyList<UnitRecord>> Query(UnitQuery query);

    /// <summary>Committed units, the ones checked longest ago first.</summary>
    Task<IReadOnlyList<UnitRecord>> LeastRecentlyVerified(int limit);

    Task MarkMissing(Id unitId);
}
