namespace Bromelia.Domain;

/// <summary>HandBrakeArgs.keepLine's state: the task being encoded (0: none yet) and the next 10 % step to keep.
/// The default value is the state before the first line.</summary>
public record struct ProgressFilter(int Task, int Next);
