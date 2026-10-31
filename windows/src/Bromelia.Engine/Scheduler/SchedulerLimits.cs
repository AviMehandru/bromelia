using System;
using Bromelia.Domain;

namespace Bromelia.Engine;

/// <summary>The configuration's limits, resolved (plan §21): <see cref="Acquisitions"/> 0 = no cap; <see cref="Cpu"/>
/// is a count (the caller turns the configuration's 0 into cores / 2: the engine can't ask the system).</summary>
public sealed record SchedulerLimits(int Acquisitions = 0, int Processing = 1, int Cpu = 2, int IoPerVolume = 2)
{
    /// <summary>The configuration's limits (config-3 Limits, its defaults where a member is missing); cpu 0 =
    /// max(1, <paramref name="cores"/> / 2). LocalEngine applies it after the load and on every configChanged.</summary>
    public static SchedulerLimits Of(Config config, int cores)
    {
        var limits = config.Document["limits"];
        int cpu = (int)(limits?["cpu"]?.AsInteger ?? 0);
        return new SchedulerLimits(
            Acquisitions: (int)(limits?["acquisitions"]?.AsInteger ?? 0),
            Processing: (int)(limits?["processing"]?.AsInteger ?? 1),
            Cpu: cpu > 0 ? cpu : Math.Max(1, cores / 2),
            IoPerVolume: (int)(limits?["ioPerVolume"]?.AsInteger ?? 2));
    }
}
