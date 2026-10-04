using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>What has been archived.</summary>
public interface ICatalogRepository
{
    /// <summary>Committed units of the same disc.</summary>
    Task<IReadOnlyList<UnitRecord>> ArchivedBefore(string fingerprint);

    /// <summary>The disc before this one of the same set.</summary>
    Task<ArchivedDisc?> PreviousDisc(ContinuationQuery query);

    Task<int?> HighestEpisode(ContinuationQuery query);

    /// <summary>Works whose title contains query, ignoring case.</summary>
    Task<IReadOnlyList<WorkRecord>> Works(string query);

    Task<DiscSetRecord?> Set(Id setId);
}
