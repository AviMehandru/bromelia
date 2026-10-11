namespace Bromelia.Engine;

/// <summary>A job's counts (dtos.json Job.counts).</summary>
public sealed record JobTotals(int Files, int Warnings, int Errors, int ReadErrors);
