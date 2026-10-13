using System.Collections.Generic;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Engine;

/// <summary>What a commit did: the unit, its folder (absolute), its files as committed, and whether they were added to
/// an existing folder.</summary>
public sealed record CommitResult(Id UnitId, string Destination, IReadOnlyList<UnitFile> Files, bool Merged);
