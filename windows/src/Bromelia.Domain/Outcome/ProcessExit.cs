using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>How a process ended: its exit status, the signal that ended it, the silence that stopped it (null
/// when it didn't stall), whether it was abandoned after KILL, and whether it was cancelled.</summary>
public sealed record ProcessExit(int Status, int? Signal = null, Duration? Stalled = null, bool Abandoned = false, bool Cancelled = false);
