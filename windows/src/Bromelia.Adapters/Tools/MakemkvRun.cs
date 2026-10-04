using Bromelia.Domain;

namespace Bromelia.Adapters;

/// <summary>One run: its outcome, the first key, version or drive notice, the LibreDrive detail, and MakeMKV's
/// version.</summary>
public sealed record MakemkvRun(
    RunOutcome Outcome,
    MakemkvNotice? Notice,
    string? LibreDrive,
    string? Version);
