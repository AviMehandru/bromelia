using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>How a process ended: its exit status, the signal that ended it, the silence that stopped it (null
/// when it didn't stall), whether it was abandoned after KILL (it may still be running and writing: what it wrote to
/// must be quarantined, never reused or removed), and whether it was cancelled (by the user or a shutdown, not by
/// the engine's own decision).</summary>
public sealed record ProcessExit(int Status, int? Signal = null, Duration? Stalled = null, bool Abandoned = false, bool Cancelled = false);
