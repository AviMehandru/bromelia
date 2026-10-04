using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>Held while the computer must stay awake.</summary>
public interface IPowerGuard
{
    void Release();
}
