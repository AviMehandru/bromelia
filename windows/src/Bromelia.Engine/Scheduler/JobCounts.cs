namespace Bromelia.Engine;

/// <summary>A count per queue.</summary>
public sealed record JobCounts(int Acquisition, int Processing, int Maintenance);
