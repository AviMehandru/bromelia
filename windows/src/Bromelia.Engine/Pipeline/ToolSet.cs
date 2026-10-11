using Bromelia.Ports;

namespace Bromelia.Engine;

/// <summary>The tools a step may run, as ports (owner, 2026-10-11). More join with their steps in phase 5.</summary>
public sealed record ToolSet(IMakemkv Makemkv, IMatroska Matroska);
