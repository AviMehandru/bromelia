namespace Bromelia.Domain;

/// <summary>The verdict of a unit's check (common.json's CheckResult).</summary>
public enum CheckResult
{
    Ok,
    Damaged,
    Error,
    Stopped,
}
