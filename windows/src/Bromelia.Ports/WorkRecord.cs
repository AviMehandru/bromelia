using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>A row of works: a movie or show.</summary>
public sealed record WorkRecord(
    Id Id,
    MediaKind Kind,
    string Title,
    int? Year,
    int? TmdbId,
    string? ImdbId,
    Instant CreatedAt,
    Instant UpdatedAt);
