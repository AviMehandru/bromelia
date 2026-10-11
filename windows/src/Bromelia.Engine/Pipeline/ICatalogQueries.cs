using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Ports;

namespace Bromelia.Engine;

/// <summary>Read-only questions about the archive (plan §12.2), over the catalogue.</summary>
public interface ICatalogQueries
{
    /// <summary>Committed units of a disc with this fingerprint.</summary>
    Task<IReadOnlyList<UnitRecord>> ArchivedBefore(string fingerprint);
}
