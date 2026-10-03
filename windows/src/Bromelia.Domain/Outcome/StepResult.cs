using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>What a finished step contributes to its job's outcome: its state and error, the read errors it saw,
/// whether it put the unit in quarantine, the MakeMKV notice that explains a failure, the reason to skip the job
/// (a disc archived before), and whether its failure counts (post-processing steps).</summary>
public sealed record StepResult(
    StepKind Kind,
    StepState State,
    BroError? Error = null,
    int ReadErrors = 0,
    bool Quarantined = false,
    MakemkvNotice? Notice = null,
    BroMessage? Skip = null,
    bool AffectsOutcome = true);
