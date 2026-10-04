using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>Finds the external tools.</summary>
public interface IToolLocator
{
    ToolInfo Locate(ToolKind tool);
}
