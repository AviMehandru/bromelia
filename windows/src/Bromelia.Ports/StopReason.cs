namespace Bromelia.Ports;

/// <summary>Why a process is stopped.</summary>
public enum StopReason
{
    Cancelled,
    Stalled,
    TimedOut,
    Shutdown,
}
