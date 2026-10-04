using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>A row of drives.</summary>
public sealed record DriveRecord(
    string Id,
    string Identification,
    string Model,
    string LastDevice,
    string? ConfigId,
    Instant FirstSeenAt,
    Instant LastSeenAt);
