namespace Bromelia.Domain;

/// <summary>Settings that change how a job's outcome is decided. None do yet: today's rules are fixed (read errors
/// quarantine, post-processing failures never fail a job). Kept so profiles can add some without changing
/// <see cref="JobOutcome.Decide"/>'s signature.</summary>
public sealed record OutcomePolicy
{
    public static readonly OutcomePolicy Default = new();
}
