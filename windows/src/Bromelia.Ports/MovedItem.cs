using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>An item moveMerging moved, and where it went.</summary>
public sealed record MovedItem(
    string From,
    string To);
