using System.Collections.Generic;

namespace Bromelia.Domain;

/// <summary>A title of a listing: MakeMKV's index (it changes with the minimum length setting), the source title
/// (24) and file (16), name (2), comment (49), duration (9), chapters (8), size (11), segment map (26), output
/// file name (27), angle (15), its tracks, and every attribute MakeMKV reported.</summary>
public sealed record Title(
    int Index,
    int? SourceTitleId,
    string SourceFile,
    string Name,
    string Comment,
    string Duration,
    int DurationSeconds,
    int Chapters,
    long SizeBytes,
    string SegmentMap,
    string OutputFileName,
    int? Angle,
    IReadOnlyList<Track> Tracks,
    IReadOnlyDictionary<int, string> Attributes);
