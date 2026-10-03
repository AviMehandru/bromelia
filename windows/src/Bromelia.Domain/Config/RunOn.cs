namespace Bromelia.Domain;

/// <summary>When a step runs: success (succeeded); failure (failed, cancelled, interrupted, or succeeded with
/// read errors); always. Skipped jobs never run steps.</summary>
public enum RunOn
{
    Success,
    Failure,
    Always,
}
