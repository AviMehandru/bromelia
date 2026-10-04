using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Adapters;

/// <summary>What a run needs besides its source: the settings to isolate, the switches (the profile path comes from
/// the lease), how long it may stay silent, and the transcript file (the job's makemkv.txt).</summary>
public sealed record MakemkvInvocation(
    MakemkvRunSettings Settings,
    MakemkvOptions Options,
    Duration? StallTimeout = null,
    string? Transcript = null);
