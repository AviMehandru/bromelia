namespace Bromelia.Engine;

/// <summary>The configuration's limits, resolved (plan §21): <see cref="Acquisitions"/> 0 = no cap; <see cref="Cpu"/>
/// is a count (the caller turns the configuration's 0 into cores / 2: the engine can't ask the system).</summary>
public sealed record SchedulerLimits(int Acquisitions = 0, int Processing = 1, int Cpu = 2, int IoPerVolume = 2);
