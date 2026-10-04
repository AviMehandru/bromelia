using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>Keeps the computer awake.</summary>
public interface IPowerManager
{
    /// <summary>Until the guard is released.</summary>
    IPowerGuard Inhibit(string reason);
}
