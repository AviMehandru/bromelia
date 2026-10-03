namespace Bromelia.Domain;

/// <summary>The status given to user scripts (BROMELIA_STATUS), notifications and the manifest.</summary>
public enum StatusWord
{
    Success,
    Errors,
    Failed,
    Cancelled,
    Skipped,
    Interrupted,
}
