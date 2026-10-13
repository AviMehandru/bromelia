using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>A row of libraries: the config entry's id, the id in its .bromelia-library marker, whether the marker was
/// there at the last check and when it was last seen.</summary>
public sealed record LibraryRecord(
    string Id,
    string Name,
    string Path,
    string MarkerId,
    Instant AttachedAt,
    bool Online,
    Instant? LastSeenAt = null);
