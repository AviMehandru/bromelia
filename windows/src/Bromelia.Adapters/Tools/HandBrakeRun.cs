using Bromelia.Domain;

namespace Bromelia.Adapters;

/// <summary>What an encode came to: HandBrakeCLI's exit, and whether the step's timeout stopped it.</summary>
public sealed record HandBrakeRun(ProcessExit Exit, bool TimedOut);
