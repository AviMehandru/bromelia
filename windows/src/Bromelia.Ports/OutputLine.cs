using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>A line of a process's output, without its line break, and when it was read.</summary>
public sealed record OutputLine(
    OutputSource Stream,
    string Text,
    Instant At);
