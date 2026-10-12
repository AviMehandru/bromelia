using Bromelia.Domain;

namespace Bromelia.Engine;

/// <summary>A connected drive as DriveService knows it: MakeMKV's index (null until a scan saw it), the OS device ("" when
/// unknown, as on macOS for an empty drive), the configuration entry that matches it, the generation of its disc
/// (null: no disc), the disc's label and MakeMKV's flags for it.</summary>
public sealed record DriveLocation(
    int? MakemkvIndex,
    string Device,
    string? ConfigId,
    long? Generation,
    string Label = "",
    DiscFlags? Flags = null);
