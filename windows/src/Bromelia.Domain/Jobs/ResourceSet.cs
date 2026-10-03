using System.Collections.Generic;

namespace Bromelia.Domain;

/// <summary>What a step holds while it runs (plan §21): drives, an acquisition slot, library writes, CPU slots
/// and I/O slots per volume. MakeMKV launches are taken inside RegistrySwapIsolation, not here.</summary>
public sealed record ResourceSet(
    IReadOnlyList<string> Drives,
    bool Acquisition,
    IReadOnlyList<string> LibraryWrites,
    int Cpu,
    IReadOnlyList<string> Io)
{
    public static readonly ResourceSet None = new(new string[0], false, new string[0], 0, new string[0]);
}
