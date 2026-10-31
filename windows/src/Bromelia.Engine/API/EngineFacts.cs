namespace Bromelia.Engine;

/// <summary>What the host knows and the engine can't ask the system for (the composition root fills it): the app's
/// version, the process id, the platform (macos, windows or linux), the data folder, the processor count
/// (limits.cpu 0 = cores / 2).</summary>
public sealed record EngineFacts(string Version, long Pid, string Platform, string DataDir, int Cores);
