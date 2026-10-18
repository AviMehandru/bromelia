using System.Collections.Generic;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>What a step's run says: its output data (for the steps after it), the steps it appends (Plan), whether it
/// skipped, what the job's outcome needs (read errors, quarantine, the MakeMKV notice that explains a failure, a reason
/// to skip the whole job, whether its failure counts), its error when it failed, and what Plan decided (the runner
/// stores it as the job's plan, with the steps appended, and its mode as the job's).</summary>
public sealed record StepReport(
    JsonValue Data,
    BroMessage? Summary = null,
    IReadOnlyList<StepKind>? AppendSteps = null,
    bool Skipped = false,
    int ReadErrors = 0,
    bool Quarantined = false,
    MakemkvNotice? Notice = null,
    BroMessage? SkipJob = null,
    BroError? Error = null,
    bool AffectsOutcome = true,
    JobPlan? Plan = null)
{
    public static StepReport Done(JsonValue data) => new(data);

    public static readonly StepReport Skip = new(JsonValue.Of(System.Array.Empty<(string, JsonValue)>()), Skipped: true);
}
