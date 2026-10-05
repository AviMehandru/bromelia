namespace Bromelia.Ports;

/// <summary>Why a process is stopped. Only Cancelled and Shutdown make its exit "cancelled"; Policy is the engine's own
/// decision (MakeMKV's space warning, a renumbered drive, an error while reading the output).</summary>
public enum StopReason
{
    Cancelled,
    Stalled,
    TimedOut,
    Shutdown,
    Policy,
}
