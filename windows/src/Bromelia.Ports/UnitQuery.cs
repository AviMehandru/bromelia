using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>Which units: of a library, matching a name, in a state; a page at a time.</summary>
public sealed record UnitQuery(
    string? LibraryId,
    string? Text,
    UnitState? State,
    int Limit,
    int Offset);
