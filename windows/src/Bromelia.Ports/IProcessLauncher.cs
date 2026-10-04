using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>Starts processes, never through a shell.</summary>
public interface IProcessLauncher
{
    /// <summary>Starts the process; fails when it can't be started.</summary>
    IRunningProcess Start(ProcessSpec spec);
}
