using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>An entry of a folder.</summary>
public sealed record DirectoryEntry(
    string Name,
    bool IsDirectory);
