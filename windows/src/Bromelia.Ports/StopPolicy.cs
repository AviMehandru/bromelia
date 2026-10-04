namespace Bromelia.Ports;

/// <summary>How a process is stopped: TERM first (makemkvcon ignores INT), or INT first (the others).</summary>
public enum StopPolicy
{
    TerminateFirst,
    InterruptFirst,
}
