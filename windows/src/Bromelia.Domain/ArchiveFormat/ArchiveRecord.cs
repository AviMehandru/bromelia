using System.Collections.Generic;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>An archive record (bromelia-&lt;unit8&gt;.json, version 3; today's bromelia.json, version 2): the document
/// as read (<see cref="Document"/>, key order kept) and the fields Bromelia reads back: status, name, kind, the
/// disc's label and place in its set (null: not in the record), its fingerprint, the unit's id (version 3), the
/// episodes' numbers and the files SHA256SUMS lists. Fields with the same meaning have the same path in both
/// versions (archive-record-3.json).</summary>
public sealed record ArchiveRecord(
    int Version,
    JsonValue Document,
    string Status,
    string Name,
    string Kind,
    string Label,
    string VolumeName,
    int? Season,
    int? Part,
    int? Volume,
    int? Disc,
    string? Fingerprint,
    string? UnitId,
    IReadOnlyList<int> Episodes,
    IReadOnlyList<SumEntry> Files);
