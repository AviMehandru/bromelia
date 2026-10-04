using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>A row of unit_files (the unit is implied).</summary>
public sealed record UnitFile(
    string Path,
    long Size,
    string Sha256,
    string Role,
    int? Title = null,
    int? Episode = null);
