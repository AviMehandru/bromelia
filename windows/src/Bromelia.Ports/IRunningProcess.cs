using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>A started process. stop escalates (INT →) TERM → KILL, 5 s apart, and abandons the process 30 s after
/// KILL. An abandoned process (ProcessExit.Abandoned) may still be running and writing: whatever it was writing to (a
/// staging folder, an image) must be quarantined, never reused or removed.</summary>
public interface IRunningProcess
{
    /// <summary>Its output lines, in order, until it closes its output.</summary>
    IAsyncEnumerable<OutputLine> Lines();

    /// <summary>Waits for it to end.</summary>
    Task<ProcessExit> Wait();

    /// <summary>Stops it (see above); wait reports the reason.</summary>
    void Stop(StopReason reason);
}
