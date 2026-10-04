using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>A timer; cancel stops it.</summary>
public interface ITimerHandle
{
    void Cancel();
}
