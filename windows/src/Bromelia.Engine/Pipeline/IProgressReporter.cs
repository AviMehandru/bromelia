using Bromelia.Domain;

namespace Bromelia.Engine;

/// <summary>How a step reports its progress, from its worker. The runner coalesces it (one update per 100 ms).</summary>
public interface IProgressReporter
{
    /// <param name="fraction">Of the step, 0–1.</param>
    /// <param name="operation">What it does.</param>
    /// <param name="current">The item it works on (MakeMKV's current operation, a file).</param>
    void Report(double fraction, BroMessage operation, string? current = null);
}
