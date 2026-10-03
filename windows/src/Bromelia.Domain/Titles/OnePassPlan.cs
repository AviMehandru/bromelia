namespace Bromelia.Domain;

/// <summary>Rip the chosen titles in one makemkvcon run, with this minimum title length (seconds).</summary>
public sealed record OnePassPlan(int MinLength);
