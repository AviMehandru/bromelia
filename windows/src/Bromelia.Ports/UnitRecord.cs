using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>A row of archive_units: one archived acquisition of a disc.</summary>
public sealed record UnitRecord(
    Id Id,
    string LibraryId,
    Id? PhysicalDiscId,
    Id? JobId,
    string Path,
    string RecordFile,
    int RecordVersion,
    UnitState State,
    UnitStatus Status,
    string Name,
    MediaKind Kind,
    string Format,
    string FormatCode,
    bool Encrypted,
    string? Fingerprint,
    string Label,
    int? Season,
    int? Part,
    int? Volume,
    int? Disc,
    string MakemkvVersion,
    long Bytes,
    int FileCount,
    int Attempts,
    Instant CreatedAt,
    Instant? CommittedAt = null);
