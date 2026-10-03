namespace Bromelia.Domain;

/// <summary>A drive as the engine knows it: its id, what MakeMKV reported and what the OS reported (either may
/// be missing).</summary>
public sealed record JoinedDrive(string DriveId, MakemkvDrive? Makemkv, OsDrive? Os);
