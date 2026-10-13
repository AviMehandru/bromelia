using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>libraries: the libraries LibraryService has seen, with their markers.</summary>
public interface ILibraryRepository
{
    /// <summary>By id.</summary>
    Task<IReadOnlyList<LibraryRecord>> All();

    Task<LibraryRecord?> Get(string libraryId);

    /// <summary>Inserts the row or replaces every column.</summary>
    Task Upsert(LibraryRecord library);

    /// <summary>Sets online; last_seen_at becomes <paramref name="at"/> only when online. Nothing when there is no such
    /// row.</summary>
    Task SetSeen(string libraryId, bool online, Instant at);
}
