using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>Where a tool is (none: not found), its version, what it can do, and why it was or wasn't found.</summary>
public sealed record ToolInfo(
    ToolKind Tool,
    string? Path,
    string? Version,
    IReadOnlyList<string> Capabilities,
    BroMessage? Why = null);
