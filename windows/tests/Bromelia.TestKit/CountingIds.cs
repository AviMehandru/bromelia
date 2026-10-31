using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.TestKit;

/// <summary>Ids that count: 00000000-0000-4000-8000-000000000001, … after the <c>given</c> ones already used (moved
/// from the PipelineRunner harness).</summary>
public sealed class CountingIds : IIdGenerator
{
    long _n;

    public CountingIds(long given = 0) { _n = given; }

    public Id Next() => new($"00000000-0000-4000-8000-{Interlocked.Increment(ref _n):x12}");

    public Id Peek() => new($"00000000-0000-4000-8000-{Interlocked.Read(ref _n) + 1:x12}");
}
